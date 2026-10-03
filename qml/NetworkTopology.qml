// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

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
    property string hoverCable: ""     // cable whose traffic card is showing
    property point hoverAt: Qt.point(0, 0)
    // Everything the selected VM can reach right now; the rest of the map fades.
    readonly property var reachFocus: selected && graph.byId[selected] && graph.byId[selected].kind === "vm" ? reachOf(selected) : null
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
    property bool exporting: false     // hides the controls while the map is saved as an image
    // "Operations center" style: a darker map where live cables glow.
    property bool operations: preferences.get("topologyStyle", "standard") === "operations"
    function setOperations(on) { operations = on; preferences.set("topologyStyle", on ? "operations" : "standard"); cables.requestPaint() }
    readonly property color mapBackground: operations ? "#04070c" : theme.colors.background

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
    // A live cable's traffic: its adapter's counters, found by host tap name, else by adapter order.
    function nicStats(c) {
        if (!c || !c.live || c.state !== "current" || !fleet || !fleet.current[c.uuid]) return null
        const nics = fleet.current[c.uuid].nics || []
        return (c.tap ? nics.find(function(n) { return n.name === c.tap }) : null) || nics.find(function(n) { return n.index === c.position }) || null
    }
    function nicHistory(c) {
        const n = nicStats(c), h = n && fleet.history[c.uuid] ? fleet.history[c.uuid].nics || {} : {}
        return n ? h[fleet.nicKey(n)] || null : null
    }
    // 0 (idle) to 1 (very busy), on a log scale from about 1 KB/s to 100 MB/s.
    function busyness(n) {
        const bytes = n ? (n.rx || 0) + (n.tx || 0) : 0
        return bytes < 64 ? 0 : Math.max(.08, Math.min(1, (Math.log(bytes) / Math.LN10 - 3) / 5))
    }
    function rateText(bytes) {
        if (bytes === null || bytes === undefined) return "–"
        const units = ["B/s", "KB/s", "MB/s", "GB/s"]
        let v = bytes, i = 0
        while (v >= 1000 && i < units.length - 1) { v /= 1000; ++i }
        return (v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)) + " " + units[i]
    }

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
            current.forEach(function(nic, position) {
                const key = String(nic.mac).toLowerCase(), saved = savedByMac[key]
                seen[key] = true
                // `tap` and `position` find this adapter's traffic counters (see nicStats()).
                cables.push({id: vm.uuid + "/" + key, vm: id, uuid: vm.uuid, mac: nic.mac, to: target(nic.networkId), networkId: nic.networkId,
                    up: !!nic.linkUp, live: live, model: nic.model, state: live && !saved ? "removing" : "current", addresses: addresses[key] || [],
                    tap: live ? String(nic.target || "") : "", position: position})
                if (live && saved && saved.networkId !== nic.networkId)
                    cables.push({id: vm.uuid + "/" + key + "/next", vm: id, uuid: vm.uuid, mac: nic.mac, to: target(saved.networkId), networkId: saved.networkId,
                        up: !!saved.linkUp, live: false, model: saved.model, state: "next"})
            })
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
    // What a VM can reach through cables that are plugged in now: its networks and the other running
    // VMs plugged into them, this computer, and the internet, with the cables and uplinks on the way.
    function reachOf(vmId) {
        const g = graph
        let r = {nodes: {}, cables: {}, uplinks: {}}
        r.nodes[vmId] = true
        const out = function(sw) {
            if (sw.uplink === "nat" || sw.uplink === "routed") { r.uplinks[sw.id + ">host"] = true; r.uplinks["host>internet"] = true; r.nodes.host = true; r.nodes.internet = true }
            else if (sw.uplink === "host") { r.uplinks[sw.id + ">host"] = true; r.nodes.host = true }
            else if (sw.uplink === "lan" || sw.uplink === "unknown") { r.uplinks[sw.id + ">internet"] = true; r.nodes.internet = true }
        }
        for (const c of g.cables) {
            if (c.vm !== vmId || c.state === "next" || !c.up) continue
            r.cables[c.id] = true
            if (c.to === "host") { r.nodes.host = true; r.nodes.internet = true; r.uplinks["host>internet"] = true; continue }
            const sw = g.byId[c.to]
            if (!sw || !sw.running) continue
            r.nodes[sw.id] = true
            out(sw)
            for (const other of g.cables)
                if (other.to === sw.id && other.vm !== vmId && other.state !== "next" && other.up && g.byId[other.vm].running) { r.cables[other.id] = true; r.nodes[other.vm] = true }
        }
        return r
    }
    onReachFocusChanged: cables.requestPaint()
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

    // How a cable is drawn: color, opacity and dashes say where it leads and whether it's plugged in.
    function cableStyle(c) {
        const dest = graph.byId[c.to]
        const ink = c.to === "host" ? theme.colors.warning : uplinkInk(dest ? dest.uplink : "")
        if (c.state === "next") return {ink: theme.colors.muted, alpha: .9, dash: [6, 7], live: false}
        if (c.state === "removing") return {ink: theme.colors.muted, alpha: .6, dash: [2, 7], live: false}
        if (!c.up) return {ink: theme.colors.danger, alpha: .9, dash: [9, 7], live: false}
        return {ink: ink, alpha: c.live ? 1 : .45, dash: null, live: c.live}
    }
    function uplinkStyle(u) {
        const ink = u.kind === "wan" ? theme.colors.warning : uplinkInk(u.kind)
        return {ink: u.live ? ink : theme.colors.muted, alpha: u.live ? .85 : .5, dash: u.kind === "lan" || !u.live ? [10, 8] : null, width: u.kind === "wan" ? 4 : 3, live: u.live}
    }
    // Each network and the VMs plugged into it share a softly tinted, titled area.
    function zones() {
        let list = []
        for (const id of graph.order) {
            const sw = graph.byId[id]
            if (sw.kind !== "switch" || !items[id]) continue
            const members = graph.cables.filter(function(c) { return c.to === id && c.state !== "next" }).map(function(c) { return items[c.vm] }).filter(function(it) { return !!it })
            if (members.length === 0) continue
            let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
            for (const it of members.concat([items[id]])) { x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y); x1 = Math.max(x1, it.x + it.width); y1 = Math.max(y1, it.y + it.height) }
            const pad = 18
            const title = sw.uplink === "none" ? (sw.sealed ? "Isolated · VMs only" : "VMs only") : sw.uplink === "host" ? "This computer only" : sw.uplink === "lan" ? "Your local network" : sw.uplink === "unknown" ? "Unknown network" : "Internet-connected"
            list.push({id: id, x: x0 - pad, y: y0 - pad - 16, w: x1 - x0 + pad * 2, h: y1 - y0 + pad * 2 + 16, ink: uplinkInk(sw.uplink), running: sw.running, title: title.toUpperCase()})
        }
        return list
    }
    function worldBounds() {
        let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
        for (const id in items) { const it = items[id]; x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y); x1 = Math.max(x1, it.x + it.width); y1 = Math.max(y1, it.y + it.height) }
        for (const z of zones()) { x0 = Math.min(x0, z.x); y0 = Math.min(y0, z.y); x1 = Math.max(x1, z.x + z.w); y1 = Math.max(y1, z.y + z.h) }
        return x0 === Infinity ? {x: 0, y: 0, w: 1, h: 1} : {x: x0, y: y0, w: x1 - x0, h: y1 - y0}
    }

    // ---- Ping walkthrough -----------------------------------------------------------------------
    // Packet Tracer-style simulation of a ping from a VM, worked out from the network settings on this
    // map; no packet is sent. Each step moves the envelope along one cable or uplink (or stays put) and
    // explains what happens there. A blocked step says why the ping goes no further.
    property var trace: null           // {title, steps: [{seg, node, text, blocked}], index}
    function destinationName(dest) { return dest === "internet" ? "the internet" : dest === "host" ? "this computer" : (graph.byId[dest] || {label: "?"}).label }
    function tracePing(fromId, dest) {
        const g = graph, vm = g.byId[fromId]
        let steps = []
        const step = function(seg, node, text, blocked) { steps.push({seg: seg, node: node, text: text, blocked: !!blocked}) }
        const title = "Ping from " + vm.label + " to " + destinationName(dest)
        step(null, fromId, vm.label + " sends a ping (an ICMP echo request) to " + destinationName(dest) + ".")
        if (!vm.running) { step(null, fromId, vm.label + " is turned off, so nothing is sent.", true); return {title: title, steps: steps, index: 0, reached: false} }
        const mine = g.cables.filter(function(c) { return c.vm === fromId && c.state !== "next" })
        const up = mine.filter(function(c) { return c.up })
        if (mine.length === 0) { step(null, fromId, vm.label + " has no network adapter, so the ping can't leave it.", true); return {title: title, steps: steps, index: 0, reached: false} }
        if (up.length === 0) { step({cable: mine[0].id}, fromId, "Every cable on " + vm.label + " is pulled, so the ping never leaves the VM.", true); return {title: title, steps: steps, index: 0, reached: false} }
        const onSwitch = function(c) { return g.byId[c.to] && g.byId[c.to].kind === "switch" ? g.byId[c.to] : null }
        const into = function(c) {
            const sw = onSwitch(c)
            step({cable: c.id}, c.to, "It leaves through adapter " + c.mac + (c.addresses && c.addresses.length ? " (" + c.addresses[0] + ")" : "")
                + (sw ? " into “" + sw.label + "”, a virtual network switch." : " into its private internet connection, which this computer runs for it."))
        }
        if (dest === "internet" || dest === "host") {
            const wantsInternet = dest === "internet"
            const ok = function(c) {
                if (c.to === "host") return true
                const sw = onSwitch(c)
                if (!sw || !sw.running) return false
                return wantsInternet ? ["nat", "routed", "lan", "unknown"].indexOf(sw.uplink) >= 0 : ["nat", "routed", "host"].indexOf(sw.uplink) >= 0
            }
            const c = up.find(ok)
            if (!c) {
                const first = up[0], sw = onSwitch(first)
                into(first)
                step(null, first.to, !sw.running ? "“" + sw.label + "” is stopped, so it doesn't pass anything on."
                    : sw.uplink === "none" ? "“" + sw.label + "” is a VMs-only network: it has no way out, so the ping stops here."
                    : sw.uplink === "host" ? "“" + sw.label + "” only reaches this computer; nothing forwards the ping to the internet."
                    : "“" + sw.label + "” has no route there, so the ping stops here.", true)
                return {title: title, steps: steps, index: 0, reached: false}
            }
            into(c)
            if (c.to === "host") step(null, "host", "This computer receives it and sends it on as if it came from this computer itself (user-mode networking).")
            else {
                const sw = onSwitch(c)
                if (sw.uplink === "lan" || sw.uplink === "unknown") {
                    step({uplink: sw.id + ">internet"}, "internet", "“" + sw.label + "” is bridged to your local network, which passes it toward the internet like any other device's traffic.")
                    step(null, "internet", (wantsInternet ? "The internet replies" : "This computer replies") + ", and the reply comes back the same way. Ping succeeds.")
                    return {title: title, steps: steps, index: 0, reached: true}
                }
                step({uplink: sw.id + ">host"}, "host", sw.uplink === "host" ? "This computer receives it on the network's bridge" + (sw.cidr ? " (" + sw.cidr.split("/")[0] + ")" : "") + "."
                    : "This computer's virtual router receives it" + (sw.cidr ? " at " + sw.cidr.split("/")[0] : "") + (wantsInternet ? " and translates the VM's address to its own (NAT)." : "."))
            }
            if (wantsInternet) step({uplink: "host>internet"}, "internet", "It goes out through your computer's internet connection to the destination.")
            step(null, wantsInternet ? "internet" : "host", (wantsInternet ? "The internet replies" : "This computer replies") + ", and the reply comes back the same way. Ping succeeds.")
            return {title: title, steps: steps, index: 0, reached: true}
        }
        // To another VM: they need a running network that both have a plugged-in cable on.
        const other = g.byId[dest]
        const theirs = g.cables.filter(function(c) { return c.vm === dest && c.state !== "next" && c.up })
        const shared = up.find(function(c) { const sw = onSwitch(c); return sw && theirs.some(function(t) { return t.to === c.to }) })
        if (!shared) {
            into(up[0])
            const mineNames = up.map(function(c) { return c.to === "host" ? "a private internet connection" : "“" + g.byId[c.to].label + "”" })
            const theirNames = theirs.map(function(c) { return c.to === "host" ? "a private internet connection" : "“" + g.byId[c.to].label + "”" })
            step(null, up[0].to, "No network connects them: " + vm.label + " is on " + mineNames.join(" and ") + ", " + other.label + " is on " + (theirNames.length ? theirNames.join(" and ") : "nothing") + ". OmaWare's networks don't route to each other, so the ping is lost.", true)
            return {title: title, steps: steps, index: 0, reached: false}
        }
        const sw = onSwitch(shared), back = theirs.find(function(t) { return t.to === shared.to })
        into(shared)
        if (!sw.running) { step(null, sw.id, "“" + sw.label + "” is stopped, so it doesn't pass anything on.", true); return {title: title, steps: steps, index: 0, reached: false} }
        step({cable: back.id, reverse: true}, dest, "The switch delivers it to " + other.label + "'s adapter" + (back.addresses && back.addresses.length ? " (" + back.addresses[0] + ")" : "") + ".")
        if (!other.running) { step(null, dest, other.label + " is turned off, so nobody answers.", true); return {title: title, steps: steps, index: 0, reached: false} }
        step(null, dest, other.label + " replies, and the reply comes back through “" + sw.label + "”. Ping succeeds — if " + other.label + "'s firewall allows pings.")
        return {title: title, steps: steps, index: 0, reached: true}
    }
    function startTrace(fromId, dest) { selected = ""; selectedCable = ""; trace = tracePing(fromId, dest) }
    function stepTrace(delta) { if (trace) trace = Object.assign({}, trace, {index: Math.max(0, Math.min(trace.steps.length - 1, trace.index + delta))}) }
    function segmentGeometry(seg) {
        if (!seg) return null
        if (seg.cable) { const c = graph.cableById[seg.cable]; return c ? geometry(c) : null }
        const u = graph.uplinks.find(function(x) { return x.id === seg.uplink })
        return u ? uplinkGeometry(u) : null
    }

    // ---- Export -------------------------------------------------------------------------------
    function esc(text) { return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;") }
    function hex(c) { const q = Qt.lighter(c, 1); const h = function(v) { return ("0" + Math.round(v * 255).toString(16)).slice(-2) }; return "#" + h(q.r) + h(q.g) + h(q.b) }
    // The whole map as a standalone SVG drawing, in the map's own coordinates.
    function svg() {
        const b = worldBounds(), pad = 30, x = Math.floor(b.x - pad), y = Math.floor(b.y - pad), w = Math.ceil(b.w + pad * 2), h = Math.ceil(b.h + pad * 2)
        const line = function(g, st, width) {
            if (!g) return ""
            return '<polyline fill="none" points="' + g.pts.map(function(q) { return q.x.toFixed(1) + "," + q.y.toFixed(1) }).join(" ") + '" stroke="' + hex(st.ink) + '" stroke-opacity="' + st.alpha
                + '" stroke-width="' + width + '" stroke-linejoin="miter"' + (st.dash ? ' stroke-dasharray="' + st.dash.join(" ") + '"' : "") + "/>"
        }
        let out = ['<svg xmlns="http://www.w3.org/2000/svg" width="' + w + '" height="' + h + '" viewBox="' + [x, y, w, h].join(" ") + '" font-family="sans-serif">',
                   '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" fill="' + hex(mapBackground) + '"/>']
        for (const z of zones()) {
            out.push('<rect x="' + z.x + '" y="' + z.y + '" width="' + z.w + '" height="' + z.h + '" rx="16" fill="' + hex(z.ink) + '" fill-opacity="0.05" stroke="' + hex(z.ink) + '" stroke-opacity="0.25"/>')
            out.push('<text x="' + (z.x + 14) + '" y="' + (z.y + 17) + '" font-size="10" font-weight="bold" letter-spacing="1" fill="' + hex(z.ink) + '">' + esc(z.title) + "</text>")
        }
        for (const u of graph.uplinks) { const st = uplinkStyle(u); out.push(line(uplinkGeometry(u), st, st.width)) }
        for (const c of graph.cables) out.push(line(geometry(c), cableStyle(c), 2.6))
        for (const id of graph.order) {
            const it = items[id]
            if (!it) continue
            const ink = hex(it.ink), rx = it.kind === "internet" ? it.height / 2 : 8
            out.push('<g opacity="' + (it.dimmed ? .7 : 1) + '"><rect x="' + it.x + '" y="' + it.y + '" width="' + it.width + '" height="' + it.height + '" rx="' + rx + '" fill="' + hex(theme.colors.surface) + '" stroke="' + hex(theme.colors.border) + '"/>')
            if (it.kind !== "internet") out.push('<rect x="' + (it.x + 1) + '" y="' + (it.y + 1) + '" width="4" height="' + (it.height - 2) + '" rx="2" fill="' + ink + '"/>')
            out.push('<text x="' + (it.x + 18) + '" y="' + (it.y + it.height / 2 - 3) + '" font-size="13" font-weight="bold" fill="' + hex(theme.colors.foreground) + '">' + esc(graph.byId[id].label) + "</text>")
            out.push('<text x="' + (it.x + 18) + '" y="' + (it.y + it.height / 2 + 13) + '" font-size="10" fill="' + hex(theme.colors.muted) + '">' + esc(it.subtitle) + "</text></g>")
        }
        out.push("</svg>")
        return out.join("\n") + "\n"
    }
    // The map as a Mermaid flowchart, for docs and wikis.
    function mermaid() {
        let ids = {}, n = 0
        const key = function(id) { if (!(id in ids)) ids[id] = "n" + (n++); return ids[id] }
        const quote = function(text) { return '"' + String(text).replace(/"/g, "#quot;") + '"' }
        let out = ["flowchart TB"]
        for (const id of graph.order) {
            const node = graph.byId[id]
            const label = node.kind === "switch" ? node.label + "<br/>" + kindText(node) + (node.cidr ? " · " + node.cidr : "")
                : node.kind === "vm" ? node.label + (node.ip ? "<br/>" + node.ip : "") + (node.running ? "" : "<br/>(stopped)") : node.label
            out.push("    " + key(id) + (node.kind === "internet" ? "((" + quote(label) + "))" : node.kind === "host" ? "[/" + quote(label) + "/]" : node.kind === "switch" ? "{{" + quote(label) + "}}" : "[" + quote(label) + "]"))
        }
        for (const u of graph.uplinks) out.push("    " + key(u.from) + (u.live ? " --- " : " -.- ") + key(u.to))
        for (const c of graph.cables) {
            if (c.state === "next") continue
            const label = !c.up ? "pulled" : c.addresses && c.addresses.length ? c.addresses[0] : ""
            out.push("    " + key(c.vm) + (c.up ? " --- " : " -.- ") + (label ? "|" + quote(label) + "| " : "") + key(c.to))
        }
        out.push("    classDef internet fill:#3b2f12,stroke:#e0a83a,color:#f6e7c8")
        out.push("    classDef local fill:#12233b,stroke:#6ea8fe,color:#dbe8ff")
        out.push("    classDef isolated fill:#10301f,stroke:#5cc98a,color:#d4f4e1")
        for (const id of graph.order) {
            const node = graph.byId[id]
            const cls = node.kind === "internet" || node.kind === "host" ? "internet" : node.kind === "switch" ? (node.uplink === "none" ? "isolated" : node.uplink === "host" ? "local" : "internet")
                : node.reach === "internet" || node.reach === "lan" ? "internet" : node.reach === "host" ? "local" : "isolated"
            out.push("    class " + key(id) + " " + cls)
        }
        return out.join("\n") + "\n"
    }
    function exportAs(kind) { exportDialog.kind = kind; exportDialog.open() }
    // Lab files: import one for review, or save a built lab's plan. The agent bridge builds labs.
    readonly property var labs: typeof agent !== "undefined" && agent ? agent : null
    function importLabFile(url) {
        const problem = labs.importLab(url)
        say(problem || "Checking the lab file…", !problem)
    }
    Connections { target: topo.labs; ignoreUnknownSignals: true; function onLabImported(ok, message) { topo.say(message, ok) } }
    FileDialog {
        id: labOpenDialog
        title: "Import a lab file"
        fileMode: FileDialog.OpenFile
        nameFilters: ["OmaWare lab files (*.toml)", "All files (*)"]
        onAccepted: topo.importLabFile(selectedFile.toString())
    }
    FileDialog {
        id: labSaveDialog
        property string slug: ""
        title: "Save the lab as a file"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "toml"
        nameFilters: ["OmaWare lab files (*.toml)"]
        currentFile: "file:" + slug + ".toml"
        onAccepted: { const ok = topo.labs.exportLab(slug, selectedFile.toString()); topo.say(ok ? "Lab saved as a file." : "The lab couldn't be saved there.", ok) }
    }
    function saveExport(url) {
        const kind = exportDialog.kind
        if (kind === "png") {
            exporting = true
            Qt.callLater(function() {
                viewport.grabToImage(function(result) {
                    const ok = result.saveToFile(preferences.localPath(url))
                    exporting = false
                    say(ok ? "Map saved as an image." : "The image couldn't be saved there.", ok)
                })
            })
            return
        }
        const ok = preferences.saveText(url, kind === "svg" ? svg() : mermaid())
        say(ok ? (kind === "svg" ? "Map saved as an SVG drawing." : "Map saved as a Mermaid diagram.") : "The file couldn't be saved there.", ok)
    }
    FileDialog {
        id: exportDialog
        property string kind: "png"
        title: kind === "png" ? "Save the map as an image" : kind === "svg" ? "Save the map as an SVG drawing" : "Save the map as a Mermaid diagram"
        fileMode: FileDialog.SaveFile
        defaultSuffix: kind === "mermaid" ? "mmd" : kind
        nameFilters: kind === "png" ? ["PNG image (*.png)"] : kind === "svg" ? ["SVG drawing (*.svg)"] : ["Mermaid diagram (*.mmd *.md)"]
        currentFile: "file:network-map." + (kind === "mermaid" ? "mmd" : kind)
        onAccepted: topo.saveExport(selectedFile.toString())
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
    // Cables are polylines of straight runs with square corners; `cum` holds the running length for point().
    function path(pts) {
        // Drop repeated points so corners stay crisp and lengths stay exact.
        let clean = []
        for (const q of pts) if (!clean.length || Math.abs(clean[clean.length - 1].x - q.x) > .01 || Math.abs(clean[clean.length - 1].y - q.y) > .01) clean.push(q)
        if (clean.length === 1) clean.push(clean[0])
        let cum = [0]
        for (let i = 1; i < clean.length; ++i) cum.push(cum[i - 1] + Math.abs(clean[i].x - clean[i - 1].x) + Math.abs(clean[i].y - clean[i - 1].y))
        return {pts: clean, cum: cum, len: Math.max(1, cum[cum.length - 1])}
    }
    // A cable leaves `a` going up and arrives at `b` from below, in horizontal and vertical runs only.
    // `bend` is the height of its horizontal run, so parallel cables can use separate lanes.
    function elbow(a, b, bend) {
        if (!a || !b) return null
        if (Math.abs(a.x - b.x) < 1) return path([a, {x: b.x, y: b.y}])
        if (a.y - b.y >= 28) {
            const y = Math.max(b.y + 14, Math.min(a.y - 14, bend === undefined ? (a.y + b.y) / 2 : bend))
            return path([a, {x: a.x, y: y}, {x: b.x, y: y}, b])
        }
        // The target is level with or below the start: climb, cross over, then come up into it.
        const top = Math.min(a.y, b.y) - 24, low = Math.max(a.y, b.y) + 24, side = (a.x + b.x) / 2
        return path([a, {x: a.x, y: top}, {x: side, y: top}, {x: side, y: low}, {x: b.x, y: low}, b])
    }
    function point(g, t) {
        const want = Math.max(0, Math.min(1, t)) * g.len
        let i = 1
        while (i < g.cum.length - 1 && g.cum[i] < want) ++i
        const span = Math.max(1e-6, g.cum[i] - g.cum[i - 1]), f = (want - g.cum[i - 1]) / span
        return {x: g.pts[i - 1].x + (g.pts[i].x - g.pts[i - 1].x) * f, y: g.pts[i - 1].y + (g.pts[i].y - g.pts[i - 1].y) * f}
    }
    // How many devices a route through these points would pass behind.
    function blocked(pts, skip) {
        let count = 0
        for (const id in items) {
            if (skip.indexOf(id) >= 0) continue
            const it = items[id], x0 = it.x - 8, y0 = it.y - 8, x1 = it.x + it.width + 8, y1 = it.y + it.height + 8
            for (let s = 1; s < pts.length; ++s) {
                const a = pts[s - 1], b = pts[s]
                // Runs are horizontal or vertical, so overlap is a box test.
                if (Math.max(a.x, b.x) > x0 && Math.min(a.x, b.x) < x1 && Math.max(a.y, b.y) > y0 && Math.min(a.y, b.y) < y1) { count++; break }
            }
        }
        return count
    }
    // Picks a square route from `a` (leaving upward) to `b` (arriving from below) that passes behind
    // as few devices as possible, then the shortest. Candidates bend at a few heights or detour
    // through the gaps beside devices. `lane` keeps parallel cables apart.
    function route(a, b, skip, lane) {
        if (!a || !b) return null
        const off = (lane || 0) * 8
        let best = null, bestCost = Infinity
        const consider = function(pts) {
            const g = path(pts), cost = blocked(g.pts, skip) * 100000 + g.len
            if (cost < bestCost) { best = g; bestCost = cost }
        }
        const climb = a.y - 22 - off, under = b.y + 22 + off
        if (a.y - b.y >= 28) for (const y of [(a.y + b.y) / 2 + off, climb, under]) consider([a, {x: a.x, y: y}, {x: b.x, y: y}, b])
        let xs = [a.x, b.x]
        for (const id in items) { const it = items[id]; xs.push(it.x - 18 - off, it.x + it.width + 18 + off) }
        for (const x of xs) consider([a, {x: a.x, y: climb}, {x: x, y: climb}, {x: x, y: under}, {x: b.x, y: under}, b])
        return best
    }
    // Routes depend on where every device is, so they are cached until something moves.
    property int layoutStamp: 0
    // Mutated in place, never reassigned, so filling it inside a binding notifies nothing.
    readonly property var routeCache: ({entries: {}, size: 0})
    function cached(key, make) {
        const store = routeCache
        if (store.size > 2000) { store.entries = {}; store.size = 0 }
        if (!(key in store.entries)) { store.entries[key] = make(); store.size++ }
        return store.entries[key]
    }
    // A cable runs from the VM's port up to the bottom of its network or this computer.
    function geometry(c) {
        const stamp = layoutStamp + registry
        return cached("c|" + c.id + "|" + c.lane + "|" + stamp, function() {
            return route(topOf(c.vm, c.lane * 16), bottomOf(c.to, c.lane * 16), [c.vm, c.to], c.lane)
        })
    }
    function uplinkGeometry(u) { return cached("u|" + u.id + "|" + (layoutStamp + registry), function() { return route(topOf(u.from), bottomOf(u.to), [u.from, u.to], 0) }) }
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
        color: topo.mapBackground
        border.color: theme.colors.border
        radius: 10
        clip: true
        onWidthChanged: { cables.requestPaint(); if (!topo.userView) fitLater.restart() }
        onHeightChanged: { cables.requestPaint(); if (!topo.userView) fitLater.restart() }

        Canvas {
            id: cables
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            // Strokes a cable solid or dashed (Canvas has no line dashes).
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
                    ctx.fillStyle = topo.operations ? topo.tint(theme.colors.accent, .16) : topo.tint(theme.colors.muted, .18)
                    for (let x = ((topo.panX % step) + step) % step; x < width; x += step)
                        for (let y = ((topo.panY % step) + step) % step; y < height; y += step) ctx.fillRect(x, y, 1.5, 1.5)
                }
                ctx.save()
                ctx.translate(topo.panX, topo.panY); ctx.scale(z, z)
                ctx.lineCap = "butt"; ctx.lineJoin = "miter"; ctx.miterLimit = 4
                // Zones: each network and the VMs plugged into it share a softly tinted, titled area.
                for (const zone of topo.zones()) {
                    ctx.beginPath(); ctx.roundedRect(zone.x, zone.y, zone.w, zone.h, 16, 16)
                    ctx.fillStyle = topo.tint(zone.ink, zone.running ? .05 : .025); ctx.fill()
                    ctx.strokeStyle = topo.tint(zone.ink, zone.running ? .22 : .1); ctx.lineWidth = 1; ctx.stroke()
                }
                const focus = topo.reachFocus
                const onPath = focus ? Object.assign({}, focus.cables, focus.uplinks) : {}
                // Uplinks: networks to this computer, this computer to the internet.
                for (const u of g.uplinks) {
                    const p = topo.uplinkGeometry(u)
                    if (!p) continue
                    const st = topo.uplinkStyle(u), ink = st.ink
                    ctx.globalAlpha = focus && !onPath[u.id] ? .18 : 1
                    if (onPath[u.id]) { ctx.strokeStyle = topo.tint(ink, .28); ctx.lineWidth = 14; stroke(ctx, p) }
                    ctx.strokeStyle = topo.tint(ink, st.alpha); ctx.lineWidth = st.width
                    if (topo.operations && st.live) { ctx.shadowColor = ink; ctx.shadowBlur = 10 }
                    stroke(ctx, p, st.dash)
                    ctx.shadowBlur = 0
                }
                // VM cables, colored by where they lead; dashes mark pulled or not-yet-applied cables.
                for (const c of g.cables) {
                    const p = topo.geometry(c)
                    if (!p) continue
                    const st = topo.cableStyle(c), ink = st.ink
                    ctx.globalAlpha = focus && !onPath[c.id] && topo.selectedCable !== c.id ? .18 : 1
                    if (onPath[c.id] || topo.selectedCable === c.id) { ctx.strokeStyle = topo.tint(theme.colors.accent, .3); ctx.lineWidth = 12; stroke(ctx, p) }
                    // Busy cables glow, brighter the more they carry.
                    const busy = c.up ? topo.busyness(topo.nicStats(c)) : 0
                    if (busy > 0) { ctx.strokeStyle = topo.tint(ink, .1 + busy * .25); ctx.lineWidth = 6 + busy * 8; stroke(ctx, p) }
                    ctx.lineWidth = 2.6; ctx.strokeStyle = topo.tint(ink, st.alpha)
                    if (topo.operations && st.live) { ctx.shadowColor = ink; ctx.shadowBlur = 8 }
                    stroke(ctx, p, st.dash)
                    ctx.shadowBlur = 0
                }
                ctx.globalAlpha = 1
                // Zone titles go on top of the cables, on a backing so a cable never runs through them.
                ctx.font = "bold 10px sans-serif"
                for (const zone of topo.zones()) {
                    const w = ctx.measureText(zone.title).width
                    ctx.fillStyle = String(topo.mapBackground); ctx.fillRect(zone.x + 10, zone.y + 6, w + 8, 15)
                    ctx.fillStyle = topo.tint(zone.ink, zone.running ? .75 : .4); ctx.fillText(zone.title, zone.x + 14, zone.y + 17)
                }
                if (topo.wire) {
                    const a = topo.topOf(topo.wire.from)
                    ctx.strokeStyle = topo.wire.over ? theme.colors.success : theme.colors.accent; ctx.lineWidth = 2.6
                    stroke(ctx, topo.elbow(a, {x: topo.wire.x, y: topo.wire.y}), [8, 6])
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
        Connections { target: topo.fleet; ignoreUnknownSignals: true; function onCurrentChanged() { cables.requestPaint() } }

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
                    onXChanged: { topo.layoutStamp++; cables.requestPaint() }
                    onYChanged: { topo.layoutStamp++; cables.requestPaint() }
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
                    readonly property var geo: cable && topo.registry >= 0 && topo.layoutStamp >= 0 ? topo.geometry(cable) : null
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
                            running: !!light.cable && light.cable.up && topo.busyness(topo.nicStats(light.cable)) > 0
                            loops: Animation.Infinite
                            NumberAnimation { to: .25; duration: 90 }
                            NumberAnimation { to: 1; duration: 140 }
                            PauseAnimation { duration: 260 }
                        }
                    }
                    Label { visible: !!light.cable && light.cable.state === "next"; anchors.centerIn: parent; text: "↻"; color: theme.colors.accent; font.pixelSize: 12; font.weight: Font.Bold }
                    opacity: topo.reachFocus && !topo.reachFocus.cables[modelData] && topo.selectedCable !== modelData ? .3 : 1
                    HoverHandler {
                        id: lightHover; cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: {
                            if (hovered) { topo.hoverCable = light.modelData; topo.hoverAt = Qt.point(topo.panX + light.cx * topo.zoom, topo.panY + light.cy * topo.zoom) }
                            else if (topo.hoverCable === light.modelData) topo.hoverCable = ""
                        }
                    }
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

            // Traffic: packets travel along each live cable while its adapter moves data. Busier cables
            // carry more, faster packets; uploads (accent) go up toward the network, downloads (green) come down.
            Repeater {
                model: topo.cableIds
                Item {
                    id: flow
                    required property string modelData
                    readonly property var cable: topo.graph.cableById[modelData] || null
                    readonly property var stats: cable && topo.fleet && topo.fleet.current ? topo.nicStats(cable) : null
                    readonly property real busy: cable && cable.up ? topo.busyness(stats) : 0
                    readonly property bool flowing: busy > 0
                    readonly property var geo: cable && topo.registry >= 0 && topo.layoutStamp >= 0 ? topo.geometry(cable) : null
                    readonly property int up: !stats ? 0 : (stats.tx || 0) < 64 ? 0 : Math.max(1, Math.round(busy * 4 * (stats.tx || 0) / Math.max(1, (stats.tx || 0) + (stats.rx || 0)) + .5))
                    readonly property int down: !stats ? 0 : (stats.rx || 0) < 64 ? 0 : Math.max(1, Math.round(busy * 4 * (stats.rx || 0) / Math.max(1, (stats.tx || 0) + (stats.rx || 0)) + .5))
                    visible: flowing && !!geo
                    opacity: topo.reachFocus && !topo.reachFocus.cables[modelData] ? .25 : 1
                    Repeater {
                        model: flow.flowing ? flow.up + flow.down : 0
                        Rectangle {
                            required property int index
                            property real t: 0
                            readonly property bool upstream: index < flow.up
                            readonly property real p: upstream ? t : 1 - t
                            width: 7; height: 7; radius: 1.5; rotation: 45
                            color: upstream ? theme.colors.accent : theme.colors.success
                            readonly property var at: flow.geo ? topo.point(flow.geo, p) : ({x: 0, y: 0})
                            x: at.x - 3.5
                            y: at.y - 3.5
                            opacity: t < .06 || t > .94 ? 0 : 1
                            NumberAnimation on t {
                                from: 0; to: 1; loops: Animation.Infinite
                                duration: Math.max(600, 3200 - flow.busy * 2600)
                                running: flow.flowing && topo.visible
                            }
                            Component.onCompleted: t = (upstream ? index / Math.max(1, flow.up) : (index - flow.up) / Math.max(1, flow.down))
                        }
                    }
                }
            }

            // Ping walkthrough: the envelope travels the current step's cable or uplink, or waits at a device.
            Item {
                id: envelope
                objectName: "pingEnvelope"
                readonly property var step: topo.trace ? topo.trace.steps[topo.trace.index] : null
                readonly property var geo: step && topo.layoutStamp >= 0 && topo.registry >= 0 ? topo.segmentGeometry(step.seg) : null
                readonly property var node: step ? topo.items[step.node] || null : null
                property real t: 1
                readonly property var at: geo && step && step.seg ? topo.point(geo, step.seg.reverse ? 1 - t : t)
                    : node ? {x: node.x + node.width / 2, y: node.y - 4} : ({x: 0, y: 0})
                visible: !!step
                x: at.x - 14; y: at.y - 10; z: 30
                Rectangle {
                    width: 28; height: 20; radius: 3
                    color: envelope.step && envelope.step.blocked ? theme.colors.danger : theme.colors.accent
                    border.color: "white"; border.width: 1.5
                    Label { anchors.centerIn: parent; text: "✉"; color: "white"; font.pixelSize: 14 }
                }
                onStepChanged: { envelopeMove.stop(); t = 0; if (geo) envelopeMove.restart(); else t = 1 }
                NumberAnimation { id: envelopeMove; target: envelope; property: "t"; from: 0; to: envelope.step && envelope.step.blocked ? .35 : 1; duration: 950; easing.type: Easing.InOutQuad }
            }

            Item {
                id: wireEnd
                x: topo.wire ? topo.wire.x : 0; y: topo.wire ? topo.wire.y : 0
            }
        }

        // ---- HUD ----
        Chip {
            visible: !topo.exporting
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
                AppButton { id: exportButton; objectName: "topologyExport"; iconName: "download"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; hint: "Export the map as an image, SVG drawing or Mermaid diagram"; onClicked: exportMenu.popup(exportButton, 0, exportButton.height) }
                AppButton { objectName: "topologyStyle"; iconName: "monitor"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; checked: topo.operations; hint: topo.operations ? "Standard map style" : "Operations-center style: dark map, glowing live cables"; onClicked: topo.setOperations(!topo.operations) }
                AppButton { objectName: "topologyInspectorToggle"; iconName: "info"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; checked: topo.inspectorOpen; hint: topo.inspectorOpen ? "Hide the side panel" : "Show the side panel"; onClicked: topo.setInspector(!topo.inspectorOpen) }
            }
        }
        AppButton {
            objectName: "killInternet"
            visible: !topo.exporting
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
            visible: !dismissed && !topo.exporting && topo.graph.order.some(function(id) { return topo.graph.byId[id].kind === "vm" })
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
        // Minimap: the whole map with the visible part outlined. Shown while the map doesn't fit; click or drag to move.
        Chip {
            id: minimap
            objectName: "topologyMinimap"
            readonly property var bounds: topo.registry >= 0 && topo.layoutStamp >= 0 ? topo.worldBounds() : ({x: 0, y: 0, w: 1, h: 1})
            readonly property real scale: Math.min((width - 12) / Math.max(1, bounds.w), (height - 12) / Math.max(1, bounds.h))
            readonly property bool overflowing: bounds.w * topo.zoom > viewport.width - 40 || bounds.h * topo.zoom > viewport.height - 40
                || topo.panX + bounds.x * topo.zoom < 0 || topo.panY + bounds.y * topo.zoom < 0
                || topo.panX + (bounds.x + bounds.w) * topo.zoom > viewport.width || topo.panY + (bounds.y + bounds.h) * topo.zoom > viewport.height
            visible: overflowing && !topo.exporting && viewport.width > 640
            anchors.right: parent.right; anchors.bottom: legendChip.top; anchors.margins: 12; anchors.bottomMargin: 8
            width: 190; height: 120
            function toWorld(px, py) { return {x: bounds.x + (px - 6) / scale, y: bounds.y + (py - 6) / scale} }
            function centerOn(px, py) {
                const w = toWorld(px, py)
                topo.userView = true; topo.panX = viewport.width / 2 - w.x * topo.zoom; topo.panY = viewport.height / 2 - w.y * topo.zoom
            }
            Canvas {
                id: miniCanvas
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d"); ctx.reset()
                    const m = minimap, w = m.bounds
                    for (const id in topo.items) {
                        const it = topo.items[id]
                        ctx.fillStyle = topo.tint(it.ink, .8)
                        ctx.fillRect(6 + (it.x - w.x) * m.scale, 6 + (it.y - w.y) * m.scale, Math.max(2, it.width * m.scale), Math.max(2, it.height * m.scale))
                    }
                    // The part of the map in view.
                    const vx = (-topo.panX / topo.zoom - w.x) * m.scale + 6, vy = (-topo.panY / topo.zoom - w.y) * m.scale + 6
                    ctx.strokeStyle = String(theme.colors.accent); ctx.lineWidth = 1.5
                    ctx.strokeRect(vx, vy, viewport.width / topo.zoom * m.scale, viewport.height / topo.zoom * m.scale)
                }
                Connections {
                    target: topo
                    function onPanXChanged() { miniCanvas.requestPaint() }
                    function onPanYChanged() { miniCanvas.requestPaint() }
                    function onZoomChanged() { miniCanvas.requestPaint() }
                    function onLayoutStampChanged() { miniCanvas.requestPaint() }
                    function onRegistryChanged() { miniCanvas.requestPaint() }
                }
                onVisibleChanged: if (visible) requestPaint()
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPressed: function(mouse) { minimap.centerOn(mouse.x, mouse.y) }
                onPositionChanged: function(mouse) { if (pressed) minimap.centerOn(mouse.x, mouse.y) }
            }
        }
        // Ping walkthrough controls: one step at a time, or play them all.
        Chip {
            id: tracePanel
            objectName: "pingTrace"
            property bool playing: false
            readonly property int count: topo.trace ? topo.trace.steps.length : 0
            readonly property var step: topo.trace ? topo.trace.steps[topo.trace.index] : null
            visible: !!topo.trace && !topo.exporting
            onVisibleChanged: if (!visible) playing = false
            z: 25
            anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 12
            width: Math.min(viewport.width - 40, 580); implicitHeight: traceColumn.implicitHeight + 22
            border.color: tracePanel.step && tracePanel.step.blocked ? theme.colors.danger : theme.colors.accent
            ColumnLayout {
                id: traceColumn
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 11
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Label { textFormat: Text.PlainText; text: topo.trace ? topo.trace.title : ""; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { text: topo.trace ? "Step " + (topo.trace.index + 1) + " of " + tracePanel.count : ""; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) }
                    AppButton { iconName: "close"; tone: "quiet"; implicitWidth: 26; implicitHeight: 26; leftPadding: 4; rightPadding: 4; hint: "End the walkthrough · Esc"; onClicked: topo.trace = null }
                }
                Label {
                    objectName: "pingTraceText"
                    textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap
                    text: tracePanel.step ? (tracePanel.step.blocked ? "✕ " : "") + tracePanel.step.text : ""
                    color: tracePanel.step && tracePanel.step.blocked ? theme.colors.danger : theme.colors.foreground
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    AppButton { text: "Back"; iconName: "previous"; tone: "quiet"; enabled: !!topo.trace && topo.trace.index > 0; onClicked: { tracePanel.playing = false; topo.stepTrace(-1) } }
                    AppButton { objectName: "pingTraceNext"; text: "Next"; iconName: "next"; enabled: !!topo.trace && topo.trace.index < tracePanel.count - 1; onClicked: { tracePanel.playing = false; topo.stepTrace(1) } }
                    AppButton { text: tracePanel.playing ? "Pause" : "Play"; iconName: tracePanel.playing ? "pause" : "play"; tone: "quiet"; enabled: !!topo.trace && (tracePanel.playing || topo.trace.index < tracePanel.count - 1); onClicked: tracePanel.playing = !tracePanel.playing }
                    Item { Layout.fillWidth: true }
                    Label { text: "Simulated from your network settings · no packet is sent"; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
                }
            }
            Timer {
                interval: 1600; repeat: true; running: tracePanel.playing
                onTriggered: { if (topo.trace && topo.trace.index < tracePanel.count - 1) topo.stepTrace(1); else tracePanel.playing = false }
            }
        }
        // Traffic card for the cable under the pointer: rates, packets, errors and the last two minutes.
        Chip {
            id: trafficCard
            objectName: "cableTrafficCard"
            readonly property var cable: topo.hoverCable ? topo.graph.cableById[topo.hoverCable] || null : null
            readonly property var stats: cable && topo.fleet && topo.fleet.current ? topo.nicStats(cable) : null
            readonly property var past: cable && topo.fleet && topo.fleet.history ? topo.nicHistory(cable) : null
            readonly property real ceiling: past ? Math.max(1024, Math.max.apply(null, (past.rx || []).concat(past.tx || []).map(function(v) { return v || 0 }))) : 1024
            visible: !!cable && !topo.exporting
            z: 20
            width: 268; implicitHeight: cardColumn.implicitHeight + 20
            x: Math.max(8, Math.min(viewport.width - width - 8, topo.hoverAt.x + 18))
            y: Math.max(8, Math.min(viewport.height - height - 8, topo.hoverAt.y - height / 2))
            ColumnLayout {
                id: cardColumn
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
                spacing: 4
                Label {
                    textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale)
                    text: !trafficCard.cable ? "" : topo.graph.byId[trafficCard.cable.vm].label + "  →  " + (trafficCard.cable.to === "host" ? "Private internet" : (topo.graph.byId[trafficCard.cable.to] || {label: "?"}).label)
                }
                Label {
                    textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale)
                    text: !trafficCard.cable ? "" : (trafficCard.cable.state === "next" ? "Waiting for the VM's next full start" : trafficCard.cable.state === "removing" ? "Removed at the next start" : !trafficCard.cable.up ? "Cable pulled" : trafficCard.cable.live ? "Connected" : "Plugged in · VM off")
                        + " · " + (trafficCard.cable.addresses && trafficCard.cable.addresses.length ? trafficCard.cable.addresses.join(", ") + " · " : "") + trafficCard.cable.mac
                }
                GridLayout {
                    visible: !!trafficCard.stats
                    columns: 2; columnSpacing: 14; rowSpacing: 1; Layout.topMargin: 4
                    Label { text: "↑ " + topo.rateText(trafficCard.stats ? trafficCard.stats.tx : null); color: theme.colors.accent; font.pixelSize: Math.round(12 * theme.textScale); font.weight: Font.DemiBold }
                    Label { text: "↓ " + topo.rateText(trafficCard.stats ? trafficCard.stats.rx : null); color: theme.colors.success; font.pixelSize: Math.round(12 * theme.textScale); font.weight: Font.DemiBold }
                    Label { text: (trafficCard.stats && trafficCard.stats.txPkts !== null ? Math.round(trafficCard.stats.txPkts) : "–") + " packets/s"; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
                    Label { text: (trafficCard.stats && trafficCard.stats.rxPkts !== null ? Math.round(trafficCard.stats.rxPkts) : "–") + " packets/s"; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
                }
                Item {
                    visible: !!trafficCard.past
                    Layout.fillWidth: true; Layout.preferredHeight: 34; Layout.topMargin: 2
                    Rectangle { anchors.fill: parent; color: "transparent"; border.color: theme.colors.border; radius: 3 }
                    Spark { anchors.fill: parent; anchors.margins: 2; values: trafficCard.past ? trafficCard.past.tx : []; capacity: topo.fleet ? topo.fleet.capacity : 60; ceiling: trafficCard.ceiling; ink: theme.colors.accent }
                    Spark { anchors.fill: parent; anchors.margins: 2; values: trafficCard.past ? trafficCard.past.rx : []; capacity: topo.fleet ? topo.fleet.capacity : 60; ceiling: trafficCard.ceiling; ink: theme.colors.success }
                }
                Label {
                    visible: !!trafficCard.stats; Layout.fillWidth: true
                    text: !trafficCard.stats ? "" : (trafficCard.stats.errorsTotal > 0 ? "⚠ " + trafficCard.stats.errorsTotal + " errors or dropped packets" : "No errors or dropped packets")
                    color: trafficCard.stats && trafficCard.stats.errorsTotal > 0 ? theme.colors.warning : theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale)
                }
                Label {
                    visible: !!trafficCard.cable && !trafficCard.stats && trafficCard.cable.live && trafficCard.cable.up
                    text: "Measuring traffic…"; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale)
                }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale); Layout.topMargin: 2
                    text: !trafficCard.cable ? "" : "Double-click to " + (trafficCard.cable.up ? "pull" : "plug in") + " · right-click for more"
                }
            }
        }
        Rectangle {
            objectName: "topologyNotice"
            visible: topo.notice !== "" && !topo.exporting
            anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 58
            width: Math.min(viewport.width - 40, noticeText.implicitWidth + 32); height: noticeText.implicitHeight + 18
            color: theme.colors.raised; radius: 8; border.color: topo.noticeOk ? theme.colors.accent : theme.colors.danger
            Label { id: noticeText; anchors.centerIn: parent; width: parent.width - 32; text: topo.notice; textFormat: Text.PlainText; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: topo.noticeOk ? theme.colors.foreground : theme.colors.danger; font.pixelSize: Math.round(12 * theme.textScale) }
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_F) { topo.refit(); event.accepted = true }
            else if (event.key === Qt.Key_Escape) { topo.wire = null; topo.selected = ""; topo.selectedCable = ""; topo.trace = null; event.accepted = true }
        }
    }

    TopologyInspector { id: inspector; map: topo; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: Math.min(340, parent.width * .34); visible: topo.inspectorOpen && topo.width > 760 }

    // ---- Menus ----
    AppMenu {
        id: exportMenu
        objectName: "topologyExportMenu"
        AppMenuItem { text: "Image (PNG)…"; onTriggered: topo.exportAs("png") }
        AppMenuItem { text: "SVG drawing…"; onTriggered: topo.exportAs("svg") }
        AppMenuItem { text: "Mermaid diagram…"; onTriggered: topo.exportAs("mermaid") }
        AppMenuItem { text: "Copy as Mermaid"; onTriggered: { preferences.copy(topo.mermaid()); topo.say("Mermaid diagram copied.", true) } }
    }
    AppMenu {
        id: canvasMenu
        objectName: "topologyCanvasMenu"
        property var at: ({x: 0, y: 0})
        AppMenuItem { text: "New network here…"; onTriggered: { topo.placement = canvasMenu.at; topo.createNetwork("nat", []) } }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { text: "Tidy up"; onTriggered: topo.arrange() }
        AppMenuItem { text: "Fit to view"; onTriggered: topo.refit() }
        AppMenuItem { text: "Export the map…"; onTriggered: exportMenu.popup() }
        AppMenuItem { text: topo.operations ? "Standard map style" : "Operations-center style"; onTriggered: topo.setOperations(!topo.operations) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { objectName: "importLabFile"; text: "Import lab file…"; enabled: !!topo.labs; onTriggered: labOpenDialog.open() }
        AppMenu {
            id: labExportMenu
            title: "Export a lab"
            enabled: !!topo.labs && topo.labs.labs.length > 0
            Instantiator {
                model: topo.labs ? topo.labs.labs : []
                AppMenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: { labSaveDialog.slug = modelData.slug; labSaveDialog.open() }
                }
                onObjectAdded: function(index, object) { labExportMenu.insertItem(index, object) }
                onObjectRemoved: function(index, object) { labExportMenu.removeItem(object) }
            }
        }
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
        AppMenu {
            id: pingMenu
            objectName: "topologyPingMenu"
            title: "Trace a ping to"
            AppMenuItem { text: "The internet"; onTriggered: topo.startTrace(vmMenu.node.id, "internet") }
            AppMenuItem { text: "This computer"; onTriggered: topo.startTrace(vmMenu.node.id, "host") }
            Instantiator {
                model: vmMenu.node.id ? topo.graph.order.filter(function(id) { return topo.graph.byId[id].kind === "vm" && id !== vmMenu.node.id }) : []
                AppMenuItem {
                    required property var modelData
                    text: topo.graph.byId[modelData] ? topo.graph.byId[modelData].label : ""
                    onTriggered: topo.startTrace(vmMenu.node.id, modelData)
                }
                onObjectAdded: function(index, object) { pingMenu.insertItem(index + 2, object) }
                onObjectRemoved: function(index, object) { pingMenu.removeItem(object) }
            }
        }
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
        MenuItem { enabled: false; contentItem: Label { textFormat: Text.PlainText; text: "Connect to " + (dropMenu.targetId === "host" ? "a private internet connection" : dropMenu.targetId && topo.graph.byId[dropMenu.targetId] ? topo.graph.byId[dropMenu.targetId].label : ""); color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
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
