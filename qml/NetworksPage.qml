// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: page
    property var catalog: backend.management["networks.list"] || ({})
    property bool mapView: true
    property var fleet: null
    property string failure: ""
    // VMs to connect once a network being created is ready.
    property var pendingVms: []
    property string status: ""
    signal openVm(string uuid)
    signal vmActions(var vm, var item, real px, real py)
    spacing: 14
    function refresh() { failure = ""; backend.request("networks.list", {}) }
    // Opens the new-network dialog, optionally with VMs already chosen (from the map or the VM list).
    function createFor(uuids) { mapView = true; networkEditor.openFor({mode: "nat", vms: uuids || []}) }
    function showVm(uuid) { mapView = true; topology.selected = "vm:" + uuid }
    onVisibleChanged: if (visible) refresh()
    // What's wrong with a network, phrased for where the fix is.
    function problemText(item) {
        if (!String(item.reason || "").startsWith("Your VMs need permission")) return item.reason || ""
        return item.managed ? "Your VMs need permission to join this network." : "Your VMs need permission to join this network. It was created outside OmaWare, so allow its bridge (" + item.bridge + ") in /etc/qemu/bridge.conf."
    }
    function plainKind(item) { return item.mode === "nat" ? "Internet + VMs" : item.mode === "hostonly" ? "This computer + VMs" : item.mode === "isolated" ? "VMs only" : item.category }
    function kindInk(item) { return item.mode === "nat" ? theme.colors.warning : item.mode === "hostonly" ? theme.colors.accent : item.mode === "isolated" ? theme.colors.success : theme.colors.muted }
    Connections { target: backend
        function onCommandFinished(op, ok, result) {
            if (op === "networks.list" && !ok) page.failure = result.message
            else if (op.indexOf("networks.") === 0 && op !== "networks.list" && ok) {
                // A network created from the map's canvas menu appears where it was requested.
                if (op === "networks.save" && result.uuid && topology.placement) {
                    let next = Object.assign({}, topology.positions)
                    next[topology.bridgeFor(result.uuid)] = {x: Math.round(topology.placement.x / 10) * 10, y: Math.round(topology.placement.y / 10) * 10}
                    topology.positions = next; topology.savePositions()
                }
                topology.placement = null
                if (op === "networks.save" && result.uuid && page.pendingVms.length) {
                    const vms = page.pendingVms
                    page.pendingVms = []
                    if (result.authorized) connectLater.start([vms, topology.bridgeFor(result.uuid)])
                    else topology.say("Network created. Allow VMs to join it, then connect them.", false)
                }
                if (result.message) topology.say(result.message, true)
                page.refresh()
            }
            else if (op.indexOf("networks.") === 0 && op !== "networks.list" && !ok) { if (op === "networks.save") page.pendingVms = []; topology.say(result.message, false) }
        }
        function onLifecycle() { if (page.visible) lifecycleRefresh.restart() }
    }
    // Connects the chosen VMs once the worker has finished creating the network.
    Timer {
        id: connectLater
        property var job: null
        interval: 150; repeat: true
        function start(args) { job = args; restart() }
        onTriggered: { if (backend.busy) return; stop(); if (job) backend.connectVms(job[0], job[1]); job = null }
    }
    Timer { id: lifecycleRefresh; interval: 500; onTriggered: page.refresh() }
    RowLayout {
        Layout.fillWidth: true
        ColumnLayout { Layout.fillWidth: true; spacing: 4
            Label { text: "Networks"; font.pixelSize: Math.round((28) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
            Label { text: page.mapView ? "How your VMs connect. Drag a VM onto a network to connect it; pull a cable to disconnect it." : "Networks your VMs can share"; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
        AppButton { iconName: "refresh"; hint: "Refresh networks"; enabled: !backend.busy; onClicked: page.refresh() }
        AppButton { objectName: "newNetwork"; text: "New network"; iconName: "plus"; tone: "primary"; enabled: !backend.busy; onClicked: networkEditor.openFor({}) }
    }
    RowLayout {
        AppButton { objectName: "labMapTab"; text: "Map"; tone: "tab"; checked: page.mapView; onClicked: page.mapView = true }
        AppButton { objectName: "networkListTab"; text: "List"; tone: "tab"; checked: !page.mapView; onClicked: page.mapView = false }
        Item { Layout.fillWidth: true }
    }
    ErrorNotice { message: page.failure; actionLabel: "Refresh networks"; onRecover: page.refresh() }
    NetworkTopology {
        id: topology
        visible: page.mapView
        Layout.fillWidth: true; Layout.fillHeight: true
        catalog: page.catalog
        fleet: page.fleet
        onOpenVm: function(uuid) { page.openVm(uuid) }
        onVmActions: function(vm, item, px, py) { page.vmActions(vm, item, px, py) }
        onEditNetwork: function(network) { networkEditor.openFor(network) }
        onCreateNetwork: function(mode, vms) { networkEditor.openFor({mode: mode, vms: vms || []}) }
        onNetworkAction: function(network, verb) { action.openFor(network, verb) }
        onRefreshRequested: page.refresh()
    }
    ScrollView {
        visible: !page.mapView
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 14
            Rectangle {
                visible: !page.mapView && (page.catalog.items || []).length === 0
                Layout.fillWidth: true; implicitHeight: emptyNetworks.implicitHeight + 44; radius: 10; color: theme.colors.surface; border.color: theme.colors.border
                ColumnLayout { id: emptyNetworks; anchors.fill: parent; anchors.margins: 22; spacing: 10
                    AppIcon { name: "network"; color: theme.colors.accent; width: 28; height: 28 }
                    Label { text: "No shared networks yet"; font.pixelSize: Math.round((20) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Label { text: "Each VM can already reach the internet on its own private connection. Create a network when you want VMs to see each other, or to keep them off the internet."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    AppButton { text: "New network"; iconName: "plus"; tone: "primary"; onClicked: networkEditor.openFor({}) }
                }
            }
            Repeater {
                model: page.mapView ? [] : (page.catalog.items || [])
                Rectangle {
                    id: networkCard
                    required property var modelData
                    readonly property var vmUsers: (modelData.users || []).concat(modelData.systemUsers || [])
                    readonly property color ink: page.kindInk(modelData)
                    Layout.fillWidth: true; implicitHeight: card.implicitHeight + 32; radius: 10; color: theme.colors.surface; border.color: theme.colors.border
                    Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.margins: 1; width: 4; radius: 2; color: networkCard.ink; opacity: modelData.active ? 1 : .4 }
                    ColumnLayout {
                        id: card; anchors.fill: parent; anchors.margins: 16; anchors.leftMargin: 20; spacing: 10
                        RowLayout {
                            spacing: 12
                            Rectangle {
                                Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: 8
                                color: Qt.rgba(networkCard.ink.r, networkCard.ink.g, networkCard.ink.b, .14)
                                AppIcon { anchors.centerIn: parent; name: modelData.mode === "isolated" ? "shield" : modelData.mode === "hostonly" ? "monitor" : "globe"; color: networkCard.ink; width: 20; height: 20 }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 2
                                Label { textFormat: Text.PlainText; text: modelData.title || String(modelData.name).replace(/^omaware-/, ""); font.pixelSize: Math.round((17) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                                Label { text: page.plainKind(modelData) + (modelData.cidr ? " · " + modelData.cidr : "") + (modelData.managed ? "" : " · managed outside OmaWare"); color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true; elide: Text.ElideRight }
                            }
                            StatusBadge { text: modelData.active ? "Running" : "Stopped"; stateCode: modelData.active ? 1 : 5 }
                        }
                        Label { text: modelData.description || ""; color: theme.colors.foreground; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * theme.textScale) }
                        // One clear problem, with its fix next to it.
                        Rectangle {
                            visible: !modelData.available && !!modelData.reason
                            Layout.fillWidth: true; implicitHeight: problemRow.implicitHeight + 18; radius: 8; color: "transparent"; border.color: theme.colors.warning
                            RowLayout {
                                id: problemRow; anchors.fill: parent; anchors.margins: 9; spacing: 10
                                Label { text: "!"; color: theme.colors.warning; font.weight: Font.Bold }
                                Label { text: page.problemText(modelData); color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * theme.textScale) }
                                AppButton { visible: modelData.managed && modelData.active && !modelData.available; text: "Allow VMs"; tone: "primary"; enabled: !backend.busy; hint: "Asks for your password once"; onClicked: action.openFor(modelData, "authorize") }
                            }
                        }
                        // Isolation: one line, with the individual checks on request.
                        RowLayout {
                            objectName: "isolation_" + modelData.name
                            visible: !!modelData.isolation && modelData.active
                            spacing: 8
                            Label { text: modelData.isolation && modelData.isolation.isolated ? "✓ Verified isolated: no path to this computer or the internet" : "✗ Isolation not verified"; color: modelData.isolation && modelData.isolation.isolated ? theme.colors.success : theme.colors.danger; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        }
                        AppDisclosure {
                            visible: !!modelData.isolation
                            objectName: "isolationChecks_" + modelData.name; title: "Isolation checks"
                            Repeater {
                                model: modelData.isolation ? modelData.isolation.checks : []
                                RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true; spacing: 8
                                    Label { text: modelData.ok ? "✓" : "✗"; color: modelData.ok ? theme.colors.success : theme.colors.danger; font.weight: Font.Bold; Layout.alignment: Qt.AlignTop }
                                    Label { text: modelData.label + (modelData.ok ? "" : " — " + modelData.detail); color: modelData.ok ? theme.colors.foreground : theme.colors.danger; font.pixelSize: Math.round(12 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Label { visible: !!modelData.isolation; text: modelData.isolation ? modelData.isolation.note : ""; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        // The VMs on it, as chips.
                        Flow {
                            Layout.fillWidth: true; spacing: 6
                            Label { text: networkCard.vmUsers.length ? "VMs:" : "No VMs connected"; color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); height: 24; verticalAlignment: Text.AlignVCenter }
                            Repeater {
                                model: networkCard.vmUsers
                                Label {
                                    required property var modelData
                                    textFormat: Text.PlainText
                                    text: String(modelData).replace(/^omaware-/, "")
                                    leftPadding: 8; rightPadding: 8; topPadding: 3; bottomPadding: 3
                                    font.pixelSize: Math.round(11 * theme.textScale)
                                    background: Rectangle { radius: 10; color: theme.colors.field; border.color: theme.colors.border }
                                }
                            }
                        }
                        AppDisclosure {
                            objectName: "networkDetails_" + modelData.name; title: "Details"
                            DetailRow { label: "Addresses"; value: modelData.cidr || "None (VMs set their own)"; Layout.fillWidth: true }
                            DetailRow { label: "Automatic addresses (DHCP)"; value: modelData.dhcp ? (modelData.dhcpStart ? modelData.dhcpStart + " – " + modelData.dhcpEnd : "On") : "Off"; Layout.fillWidth: true }
                            DetailRow { label: "Starts with the computer"; value: modelData.autostart ? "Yes" : "No"; Layout.fillWidth: true }
                            DetailRow { label: "Bridge"; value: modelData.bridge || "None"; Layout.fillWidth: true }
                            DetailRow { label: "Name in libvirt"; value: modelData.name || ""; Layout.fillWidth: true }
                        }
                        Flow {
                            visible: modelData.managed
                            Layout.fillWidth: true; spacing: 6
                            AppButton { text: modelData.active ? "Stop" : "Start"; enabled: !backend.busy && (!modelData.active || networkCard.vmUsers.length === 0); hint: modelData.active ? (networkCard.vmUsers.length ? "Disconnect its VMs first" : "Stop this network") : "Start this network"; onClicked: action.openFor(modelData, modelData.active ? "stop" : "start") }
                            AppButton { text: "Edit"; enabled: !backend.busy && !modelData.active && networkCard.vmUsers.length === 0; hint: "Stop the network and disconnect its VMs to edit it"; onClicked: networkEditor.openFor(modelData) }
                            AppButton { text: "Show on map"; tone: "quiet"; onClicked: { page.mapView = true; topology.selected = topology.bridgeFor(modelData.uuid) } }
                            AppButton { text: "Remove"; tone: "quiet"; enabled: !backend.busy && !modelData.active && networkCard.vmUsers.length === 0; onClicked: action.openFor(modelData, "remove") }
                        }
                    }
                }
            }
            AppDisclosure {
                objectName: "networkHelp"; title: "Which kind of network do I need?"
                Label { text: "• Internet + VMs: VMs can reach each other, this computer and the internet. The usual choice."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• This computer + VMs: like above, but without the internet. Good for testing servers from your computer."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• VMs only: VMs can only reach each other. Nothing gets in or out; OmaWare checks this live."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "Without any network, each VM can still use its own private internet connection. Creating a network never changes your computer's own connection."; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
            }
        }
    }
    EditorDialog {
        id: networkEditor
        objectName: "hostNetworkEditor"
        property var original: ({})
        property int modeIndex: 0
        property var chosenVms: ({})
        readonly property bool editing: !!original.uuid
        // Taken when the dialog opens: a list that changes while the dialog is laid out can crash Qt 6.4's layouts.
        property var candidates: []
        headerIcon: "network"
        subtitle: editing ? "Change this network's addresses" : "A network VMs can share"
        heading: editing ? "Edit network" : "New network"
        actionText: editing ? "Save" : "Create network"
        function openFor(data) {
            networkAddresses.expanded = false; original = data
            networkName.text = data.title || (data.name || "").replace(/^omaware-/, "")
            modeIndex = Math.max(0, ["nat", "hostonly", "isolated"].indexOf(data.mode || "nat"))
            subnet.text = data.cidr || ""; dhcp.checked = data.dhcp === undefined ? true : data.dhcp
            dhcpStart.text = data.dhcpStart || ""; dhcpEnd.text = data.dhcpEnd || ""
            autostart.checked = data.autostart === undefined ? true : data.autostart
            let chosen = {}
            for (const uuid of (data.vms || [])) chosen[uuid] = true
            chosenVms = chosen
            candidates = backend.domains.filter(function(vm) { return vm.owned }).map(function(vm) { return {uuid: vm.uuid, name: vm.name, stateCode: vm.stateCode, contained: vm.contained} })
            open()
        }
        readonly property var modes: [
            {key: "nat", icon: "globe", title: "Internet", text: "VMs can reach each other, this computer and the internet.", ink: theme.colors.warning},
            {key: "hostonly", icon: "monitor", title: "This computer", text: "VMs can reach each other and this computer. No internet.", ink: theme.colors.accent},
            {key: "isolated", icon: "shield", title: "Only each other", text: "VMs can only reach each other. Nothing gets in or out.", ink: theme.colors.success}]
        Label { text: "Name" }
        AppField { id: networkName; objectName: "hostNetworkName"; Layout.fillWidth: true; placeholderText: "For example: Home lab"; readOnly: networkEditor.editing }
        Label { text: "What can VMs on it reach?" }
        // Three plain choices instead of NAT / host-only / isolated jargon.
        RowLayout {
            id: mode
            objectName: "hostNetworkMode"
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                model: networkEditor.modes
                Rectangle {
                    id: choice
                    required property var modelData
                    required property int index
                    readonly property bool picked: networkEditor.modeIndex === index
                    objectName: "networkMode_" + modelData.key
                    Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true; Layout.preferredHeight: choiceColumn.implicitHeight + 24
                    radius: 10
                    color: picked ? Qt.rgba(Qt.lighter(modelData.ink, 1).r, Qt.lighter(modelData.ink, 1).g, Qt.lighter(modelData.ink, 1).b, .12) : theme.colors.field
                    border.width: picked ? 2 : 1
                    border.color: picked ? modelData.ink : choiceHover.hovered ? theme.colors.muted : theme.colors.border
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.title + ". " + modelData.text
                    Accessible.checked: picked
                    ColumnLayout {
                        id: choiceColumn; anchors.fill: parent; anchors.margins: 12; spacing: 6
                        AppIcon { name: choice.modelData.icon; color: choice.modelData.ink; width: 22; height: 22 }
                        Label { text: choice.modelData.title; font.weight: Font.DemiBold; font.pixelSize: Math.round(14 * theme.textScale); Layout.fillWidth: true }
                        Label { text: choice.modelData.text; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                    HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: networkEditor.modeIndex = choice.index }
                }
            }
        }
        // Which VMs to connect straight away.
        ColumnLayout {
            visible: !networkEditor.editing && networkEditor.candidates.length > 0
            Layout.fillWidth: true; spacing: 4
            Label { text: "Connect these VMs (optional)" }
            Repeater {
                model: networkEditor.candidates
                AppCheckBox {
                    required property var modelData
                    visible: networkEditor.modeIndex === 2 || !modelData.contained
                    objectName: "connectVm_" + modelData.uuid
                    text: String(modelData.name).replace(/^omaware-/, "") + (modelData.stateCode === 1 ? "  ·  running" : "")
                    checked: !!networkEditor.chosenVms[modelData.uuid]
                    onToggled: { let next = Object.assign({}, networkEditor.chosenVms); if (checked) next[modelData.uuid] = true; else delete next[modelData.uuid]; networkEditor.chosenVms = next }
                }
            }
            Label { text: "Each gets an extra connection to the new network, straight away if it's running. Its other connections stay as they are."; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        AppDisclosure {
            id: networkAddresses; objectName: "networkAddresses"; title: "Address settings"; visible: networkEditor.modeIndex !== 2
            Label { text: "Subnet" }
            AppField { id: subnet; Layout.fillWidth: true; placeholderText: "Automatic (a free 192.168.x.0/24)" }
            AppCheckBox { id: dhcp; text: "Give VMs addresses automatically (DHCP)" }
            RowLayout { visible: dhcp.checked; Layout.fillWidth: true
                AppField { id: dhcpStart; Layout.fillWidth: true; placeholderText: "First address (automatic)" }
                Label { text: "to" }
                AppField { id: dhcpEnd; Layout.fillWidth: true; placeholderText: "Last address (automatic)" }
            }
            AppCheckBox { id: autostart; text: "Start this network when the computer starts" }
            Label { text: "Your computer takes the first address in the subnet. Subnets already in use are refused."; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        Label {
            text: networkEditor.modeIndex === 2 ? "VMs on it get no automatic addresses: give them fixed addresses on one subnet, or run a DHCP server in one of them. OmaWare checks the isolation live once it's running." : ""
            visible: text !== ""; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale)
        }
        Label { visible: !networkEditor.editing; text: "You'll be asked for your password once, so your VMs are allowed to join the new network."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) }
        onSubmitted: {
            page.pendingVms = Object.keys(chosenVms)
            execute("networks.save", {uuid: original.uuid || "", revision: original.revision || "", name: networkName.text.trim() || "Network", mode: ["nat", "hostonly", "isolated"][modeIndex],
                subnet: subnet.text.trim(), dhcp: dhcp.checked, dhcpStart: dhcpStart.text.trim(), dhcpEnd: dhcpEnd.text.trim(), autostart: autostart.checked, start: true, authorize: !editing})
        }
    }
    EditorDialog {
        id: action
        height: Math.min(400, parent.height - 40)
        headerIcon: "network"
        property var network: ({})
        property string verb: ""
        function openFor(data, operation) { network = data; verb = operation; open() }
        heading: verb === "authorize" ? "Allow your VMs to join?" : verb === "remove" ? "Remove this network?" : verb === "stop" ? "Stop this network?" : "Start this network?"
        actionText: verb === "authorize" ? "Allow VMs" : verb === "remove" ? "Remove network" : verb === "stop" ? "Stop network" : "Start network"
        actionTone: verb === "remove" ? "danger" : "primary"
        Label { text: action.network.title || String(action.network.name || "").replace(/^omaware-/, ""); font.pixelSize: Math.round((18) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Label {
            text: action.verb === "authorize" ? "Your VMs run as your user, so they need permission to join this network. You'll be asked for your password once; only this network is affected."
                : action.verb === "remove" ? "The network is deleted. Move its VMs to another network first." : action.verb === "stop" ? "VMs can't use this network while it's stopped." : "VMs can use this network once it's running."
            color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap
        }
        onSubmitted: execute("networks." + verb, {uuid: network.uuid, revision: network.revision})
    }
}
