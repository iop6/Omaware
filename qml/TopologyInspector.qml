// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Side panel for the network map: what the selected device can reach, its addresses, and the controls for it.
Rectangle {
    id: panel
    objectName: "topologyInspector"
    property var map: null
    readonly property var g: map ? map.graph : ({
            byId: {},
            order: [],
            cables: []
        })
    readonly property var node: map && map.selected ? g.byId[map.selected] || null : null
    readonly property var cable: map && map.selectedCable ? g.cableById[map.selectedCable] || null : null
    readonly property var vms: g.order.map(function (id) {
        return g.byId[id];
    }).filter(function (n) {
        return n.kind === "vm";
    })
    readonly property var networks: g.order.map(function (id) {
        return g.byId[id];
    }).filter(function (n) {
        return n.kind === "switch";
    })
    function cablesOf(id) {
        return g.cables.filter(function (c) {
            return c.vm === id || c.to === id;
        });
    }
    function targetLabel(c) {
        return c.to === "host" ? "Private internet" : (g.byId[c.to] || {
                label: c.to
            }).label;
    }
    function targetKind(c) {
        return c.to === "host" ? "Internet · only this VM" : map ? map.kindText(g.byId[c.to]) : "";
    }
    function cableInk(c) {
        return c.to === "host" ? theme.colors.warning : map ? map.uplinkInk((g.byId[c.to] || {}).uplink) : theme.colors.muted;
    }
    function cableState(c) {
        return c.state === "next" ? "Takes effect at the next full start" : c.state === "removing" ? "Removed at the next full start" : !c.up ? "Cable pulled" : c.live ? "Connected" : "Connects when the VM starts";
    }
    color: theme.colors.surface
    border.color: theme.colors.border
    radius: 10

    component Heading: Label {
        color: theme.colors.muted
        font.pixelSize: Math.round(10 * theme.textScale)
        font.weight: Font.Bold
        font.letterSpacing: 1.2
        Layout.topMargin: 10
    }
    component Line: Label {
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        font.pixelSize: Math.round(12 * theme.textScale)
    }
    // A colored card stating a verdict in words, e.g. "Online · can reach the internet".
    component Verdict: Rectangle {
        id: verdictCard
        property color ink: theme.colors.muted
        property string title: ""
        property string text: ""
        property var lines: []
        Layout.fillWidth: true
        implicitHeight: verdictColumn.implicitHeight + 22
        radius: 8
        color: Qt.rgba(Qt.lighter(ink, 1).r, Qt.lighter(ink, 1).g, Qt.lighter(ink, 1).b, .1)
        border.color: Qt.rgba(Qt.lighter(ink, 1).r, Qt.lighter(ink, 1).g, Qt.lighter(ink, 1).b, .45)
        ColumnLayout {
            id: verdictColumn
            anchors.fill: parent
            anchors.margins: 11
            spacing: 4
            Label {
                text: verdictCard.title
                color: verdictCard.ink
                font.weight: Font.Bold
                font.pixelSize: Math.round(15 * theme.textScale)
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                visible: text !== ""
                text: verdictCard.text
                color: theme.colors.foreground
                font.pixelSize: Math.round(12 * theme.textScale)
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: verdictCard.lines
                Label {
                    required property var modelData
                    textFormat: Text.PlainText
                    text: "↳ " + modelData
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
    // One connection of a VM (or one VM on a network), with its address and a pull/plug button.
    component CableRow: Rectangle {
        id: row
        property var cable: null
        property var owner: null   // inline components do not see the file's ids
        property bool fromVm: true
        readonly property var vmNode: owner && cable ? owner.g.byId[cable.vm] || ({}) : ({})
        Layout.fillWidth: true
        implicitHeight: rowLayout.implicitHeight + 16
        radius: 8
        color: theme.colors.field
        border.color: theme.colors.border
        RowLayout {
            id: rowLayout
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            spacing: 9
            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: !row.cable ? "transparent" : !row.cable.up ? theme.colors.danger : row.owner.cableInk(row.cable)
                opacity: row.cable && (row.cable.live || !row.cable.up) ? 1 : .5
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    textFormat: Text.PlainText
                    text: (!row.owner || !row.cable || !row.cable.to ? "" : row.fromVm ? row.owner.targetLabel(row.cable) : row.vmNode.label) || ""
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    font.pixelSize: Math.round(12 * theme.textScale)
                }
                Label {
                    textFormat: Text.PlainText
                    text: !row.cable ? "" : row.owner.cableState(row.cable) + (row.fromVm ? " · " + row.owner.targetKind(row.cable) : "")
                    color: row.cable && !row.cable.up ? theme.colors.danger : theme.colors.muted
                    font.pixelSize: Math.round(10 * theme.textScale)
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "address_" + (row.cable ? row.cable.id : "")
                    visible: text !== ""
                    textFormat: Text.PlainText
                    text: !row.cable || !row.cable.live ? "" : row.cable.addresses && row.cable.addresses.length ? "IP " + row.cable.addresses.join(", ") : row.cable.to === "host" ? "Private address" : row.cable.up ? "IP not known yet" : ""
                    color: row.cable && row.cable.addresses && row.cable.addresses.length ? theme.colors.foreground : theme.colors.muted
                    ToolTip.visible: addressHover.hovered && text === "Private address"
                    ToolTip.text: "The VM has its own private internet connection, so this computer can't reach it directly."
                    HoverHandler {
                        id: addressHover
                    }
                    font.pixelSize: Math.round(11 * theme.textScale)
                    font.family: "monospace"
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            AppButton {
                visible: !!row.cable && !!row.cable.addresses && row.cable.addresses.length > 0 && row.cable.live
                iconName: "clipboard"
                tone: "quiet"
                implicitWidth: 28
                implicitHeight: 28
                leftPadding: 6
                rightPadding: 6
                hint: "Copy IP address"
                onClicked: preferences.copy(row.cable.addresses[0])
            }
            AppButton {
                visible: !!row.cable && row.cable.state !== "next" && !!row.vmNode.owned
                text: row.cable && row.cable.up ? "Pull" : "Plug in"
                tone: row.cable && row.cable.up ? "quiet" : "primary"
                iconName: row.cable && row.cable.up ? "unplug" : "plug"
                implicitHeight: 30
                hint: row.cable && row.cable.up ? "Disconnect this cable now" : "Reconnect this cable now"
                onClicked: row.owner.map.setLinks([row.cable], !row.cable.up)
            }
        }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar {}
        ColumnLayout {
            id: body
            width: parent.width
            spacing: 8

            // ---- Nothing selected: everything at a glance ----
            ColumnLayout {
                visible: !panel.node && !panel.cable
                Layout.fillWidth: true
                spacing: 8
                Heading {
                    text: "OVERVIEW"
                    Layout.topMargin: 0
                }
                Verdict {
                    ink: panel.g.exposed > 0 ? theme.colors.warning : theme.colors.success
                    title: panel.g.exposed > 0 ? panel.g.exposed + (panel.g.exposed === 1 ? " running VM is online" : " running VMs are online") : "No running VM can reach the internet"
                    text: panel.g.exposed > 0 ? "Select a VM to see how it connects, or use Cut off internet to disconnect them all at once." : "Running VMs can only reach this computer or each other, or nothing at all."
                }
                Heading {
                    text: "VIRTUAL MACHINES"
                }
                Repeater {
                    model: panel.vms
                    ItemDelegate {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 42
                        padding: 6
                        background: Rectangle {
                            color: parent.hovered ? theme.colors.subtle : "transparent"
                            radius: 6
                        }
                        contentItem: RowLayout {
                            spacing: 9
                            AppIcon {
                                name: "monitor"
                                width: 16
                                height: 16
                                color: modelData.running ? theme.colors.foreground : theme.colors.muted
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    textFormat: Text.PlainText
                                    text: modelData.label
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    font.pixelSize: Math.round(12 * theme.textScale)
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: modelData.running ? (modelData.ip || "Running") : "Stopped"
                                    color: theme.colors.muted
                                    font.pixelSize: Math.round(10 * theme.textScale)
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                text: panel.map ? panel.map.reachInfo[modelData.reach].label : ""
                                color: panel.map ? panel.map.reachInfo[modelData.reach].ink : "transparent"
                                font.pixelSize: Math.round(9 * theme.textScale)
                                font.weight: Font.Bold
                            }
                        }
                        onClicked: panel.map.selected = modelData.id
                    }
                }
                Line {
                    visible: panel.vms.length === 0
                    text: "No VMs yet."
                    color: theme.colors.muted
                }
                Heading {
                    text: "NETWORKS"
                }
                Repeater {
                    model: panel.networks
                    ItemDelegate {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 42
                        padding: 6
                        background: Rectangle {
                            color: parent.hovered ? theme.colors.subtle : "transparent"
                            radius: 6
                        }
                        contentItem: RowLayout {
                            spacing: 9
                            Rectangle {
                                width: 10
                                height: 10
                                radius: 5
                                color: panel.map ? panel.map.uplinkInk(modelData.uplink) : "transparent"
                                opacity: modelData.running ? 1 : .4
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    textFormat: Text.PlainText
                                    text: modelData.label
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    font.pixelSize: Math.round(12 * theme.textScale)
                                }
                                Label {
                                    text: (panel.map ? panel.map.kindText(modelData) : "") + (modelData.running ? "" : " · stopped")
                                    color: theme.colors.muted
                                    font.pixelSize: Math.round(10 * theme.textScale)
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                text: modelData.vmCount + (modelData.vmCount === 1 ? " VM" : " VMs")
                                color: theme.colors.muted
                                font.pixelSize: Math.round(10 * theme.textScale)
                            }
                        }
                        onClicked: panel.map.selected = modelData.id
                    }
                }
                Line {
                    visible: panel.networks.length === 0
                    text: "No shared networks yet."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                AppButton {
                    objectName: "inspectorNewNetwork"
                    text: "New network"
                    iconName: "plus"
                    tone: "primary"
                    Layout.topMargin: 4
                    onClicked: panel.map.createNetwork("nat", [])
                }
            }

            // ---- A cable ----
            ColumnLayout {
                visible: !!panel.cable && !panel.node
                Layout.fillWidth: true
                spacing: 8
                Heading {
                    text: "CONNECTION"
                    Layout.topMargin: 0
                }
                Line {
                    text: panel.cable ? (panel.g.byId[panel.cable.vm] || {
                            label: ""
                        }).label + "  →  " + panel.targetLabel(panel.cable) : ""
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(16 * theme.textScale)
                }
                CableRow {
                    owner: panel
                    visible: !!panel.cable
                    cable: panel.cable || ({
                            mac: "",
                            model: "",
                            up: false,
                            live: false,
                            state: "current",
                            addresses: []
                        })
                }
                Line {
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                    text: !panel.cable ? "" : panel.cable.state === "next" ? "This connection is saved, but the running VM couldn't switch to it. It takes effect after a full shutdown and start." : panel.cable.live ? "Pulling it disconnects the VM from this network immediately, as if you unplugged a real cable. It stays unplugged after a restart." : "The VM is stopped. This is how it connects when it starts."
                }
                Line {
                    visible: !!panel.cable
                    text: panel.cable ? "Adapter " + panel.cable.mac + " · " + panel.cable.model : ""
                    color: theme.colors.muted
                    font.pixelSize: Math.round(10 * theme.textScale)
                }
            }

            // ---- A VM ----
            ColumnLayout {
                id: vmSection
                visible: !!panel.node && panel.node.kind === "vm"
                Layout.fillWidth: true
                spacing: 8
                readonly property var info: panel.node && panel.node.kind === "vm" && panel.map ? panel.map.reachInfo[panel.node.reach] : ({
                        label: "",
                        title: "",
                        text: "",
                        ink: "transparent"
                    })
                readonly property var mine: panel.node && panel.node.kind === "vm" ? panel.cablesOf(panel.node.id) : []
                Heading {
                    text: "VIRTUAL MACHINE"
                    Layout.topMargin: 0
                }
                Line {
                    text: panel.node ? panel.node.label : ""
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(18 * theme.textScale)
                }
                Line {
                    text: panel.node ? panel.node.state + (panel.node.contained ? " · contained" : "") : ""
                    color: panel.node && panel.node.contained ? theme.colors.danger : theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Verdict {
                    objectName: "inspectorReach"
                    ink: vmSection.info.ink
                    title: vmSection.info.title + (panel.node && !panel.node.running ? " when it starts" : "")
                    text: vmSection.info.text + "."
                    lines: panel.node && panel.node.via ? panel.node.via : []
                }
                Heading {
                    text: "CONNECTIONS"
                }
                Repeater {
                    model: vmSection.mine
                    CableRow {
                        required property var modelData
                        owner: panel
                        cable: modelData
                    }
                }
                Line {
                    visible: vmSection.mine.length === 0
                    text: "Not connected. Drag its + onto a network."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Layout.topMargin: 6
                    AppButton {
                        text: "Open"
                        iconName: "terminal"
                        tone: "primary"
                        hint: "Open the VM's console"
                        onClicked: panel.map.openVm(panel.node.uuid)
                    }
                    AppButton {
                        id: connectButton
                        objectName: "inspectorConnect"
                        text: "Connect to"
                        iconName: "plug"
                        visible: !!panel.node && !!panel.node.owned
                        onClicked: connectMenu.popup(connectButton, 0, connectButton.height)
                        AppMenu {
                            id: connectMenu
                            Instantiator {
                                model: panel.node && panel.node.kind === "vm" && panel.map ? panel.map.switchesFor(panel.node) : []
                                AppMenuItem {
                                    required property var modelData
                                    text: modelData.label
                                    enabled: modelData.ok
                                    onTriggered: panel.map.cableTo(panel.node.id, modelData.id, "")
                                }
                                onObjectAdded: function (index, object) {
                                    connectMenu.insertItem(index, object);
                                }
                                onObjectRemoved: function (index, object) {
                                    connectMenu.removeItem(object);
                                }
                            }
                            MenuSeparator {
                                contentItem: Rectangle {
                                    implicitHeight: 1
                                    color: theme.colors.border
                                }
                            }
                            AppMenuItem {
                                text: "New network with this VM…"
                                onTriggered: panel.map.createNetwork("nat", [panel.node.uuid])
                            }
                        }
                    }
                    AppButton {
                        objectName: "inspectorAirGap"
                        text: "Disconnect"
                        iconName: "unplug"
                        tone: "danger"
                        visible: !!panel.node && !!panel.node.owned
                        enabled: vmSection.mine.some(function (c) {
                            return c.vm === panel.node.id && c.up;
                        })
                        hint: "Pull every cable on this VM, now and after restarts"
                        onClicked: panel.map.setLinks(vmSection.mine, false)
                    }
                }
            }

            // ---- A network ----
            ColumnLayout {
                id: netSection
                visible: !!panel.node && panel.node.kind === "switch"
                Layout.fillWidth: true
                spacing: 8
                readonly property var sw: panel.node && panel.node.kind === "switch" ? panel.node : ({})
                readonly property var net: sw.network || null
                readonly property int users: net ? (net.users || []).length + (net.systemUsers || []).length : 0
                readonly property var mine: panel.node && panel.node.kind === "switch" ? panel.cablesOf(panel.node.id) : []
                Heading {
                    text: "NETWORK"
                    Layout.topMargin: 0
                }
                Line {
                    text: netSection.sw.label || ""
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(18 * theme.textScale)
                }
                Line {
                    text: netSection.sw.running ? "Running" : "Stopped"
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Verdict {
                    ink: panel.map ? panel.map.uplinkInk(netSection.sw.uplink) : theme.colors.muted
                    title: panel.map ? panel.map.kindText(netSection.sw) : ""
                    text: netSection.sw.description || ""
                }
                // One clear problem, with the fix right next to it.
                Rectangle {
                    visible: !!netSection.sw.reason
                    Layout.fillWidth: true
                    implicitHeight: problem.implicitHeight + 20
                    radius: 8
                    color: "transparent"
                    border.color: theme.colors.warning
                    ColumnLayout {
                        id: problem
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8
                        Line {
                            text: !netSection.sw.needsPermission ? netSection.sw.reason || "" : netSection.sw.managed ? "Your VMs need permission to join this network." : "Your VMs need permission to join this network. It was created outside OmaWare, so allow its bridge (" + netSection.sw.bridge + ") in /etc/qemu/bridge.conf."
                            color: theme.colors.warning
                            font.pixelSize: Math.round(11 * theme.textScale)
                        }
                        AppButton {
                            visible: !!netSection.net && netSection.sw.managed && netSection.sw.running && netSection.sw.needsPermission
                            text: "Allow VMs to join"
                            tone: "primary"
                            hint: "Asks for your password once"
                            onClicked: panel.map.networkAction(netSection.net, "authorize")
                        }
                        AppButton {
                            visible: !!netSection.net && netSection.sw.managed && !netSection.sw.running
                            text: "Start network"
                            tone: "primary"
                            enabled: !backend.busy
                            onClicked: panel.map.networkAction(netSection.net, "start")
                        }
                    }
                }
                RowLayout {
                    visible: netSection.sw.category === "Isolated" && netSection.sw.running
                    spacing: 8
                    Label {
                        text: netSection.sw.sealed ? "✓ Verified isolated" : "✗ Isolation not verified"
                        color: netSection.sw.sealed ? theme.colors.success : theme.colors.danger
                        font.weight: Font.Bold
                        font.pixelSize: Math.round(12 * theme.textScale)
                    }
                    AppButton {
                        text: checks.visible ? "Hide checks" : "Show checks"
                        tone: "quiet"
                        implicitHeight: 26
                        onClicked: checks.visible = !checks.visible
                    }
                }
                ColumnLayout {
                    id: checks
                    visible: false
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: netSection.sw.isolation ? netSection.sw.isolation.checks : []
                        RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                text: modelData.ok ? "✓" : "✗"
                                color: modelData.ok ? theme.colors.success : theme.colors.danger
                                font.weight: Font.Bold
                                Layout.alignment: Qt.AlignTop
                            }
                            Line {
                                text: modelData.label + (modelData.ok ? "" : " — " + modelData.detail)
                                color: modelData.ok ? theme.colors.foreground : theme.colors.danger
                                font.pixelSize: Math.round(11 * theme.textScale)
                            }
                        }
                    }
                }
                DetailRow {
                    visible: !!netSection.sw.cidr
                    label: "Addresses"
                    value: netSection.sw.cidr || ""
                    Layout.fillWidth: true
                }
                Heading {
                    text: "VMS ON THIS NETWORK"
                }
                Repeater {
                    model: netSection.mine
                    CableRow {
                        required property var modelData
                        owner: panel
                        cable: modelData
                        fromVm: false
                    }
                }
                Line {
                    visible: netSection.mine.length === 0
                    text: "No VMs yet. Drag the ● on a VM onto this network to connect it."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Layout.topMargin: 6
                    AppButton {
                        visible: !!netSection.net && netSection.sw.managed && netSection.sw.running
                        text: "Stop"
                        enabled: !backend.busy && netSection.users === 0
                        hint: netSection.users ? "Disconnect its VMs first" : "Stop this network"
                        onClicked: panel.map.networkAction(netSection.net, "stop")
                    }
                    AppButton {
                        visible: !!netSection.net && netSection.sw.managed
                        text: "Edit"
                        enabled: !backend.busy && !netSection.sw.running && netSection.users === 0
                        hint: "Stop the network and disconnect its VMs to edit it"
                        onClicked: panel.map.editNetwork(netSection.net)
                    }
                    AppButton {
                        visible: !!netSection.net && netSection.sw.managed
                        text: "Remove"
                        tone: "quiet"
                        enabled: !backend.busy && !netSection.sw.running && netSection.users === 0
                        onClicked: panel.map.networkAction(netSection.net, "remove")
                    }
                    AppButton {
                        text: "Disconnect all"
                        iconName: "unplug"
                        tone: "danger"
                        enabled: netSection.mine.some(function (c) {
                            return c.up;
                        })
                        hint: "Pull every cable on this network"
                        onClicked: panel.map.setLinks(netSection.mine, false)
                    }
                }
            }

            // ---- This computer / the internet ----
            ColumnLayout {
                visible: !!panel.node && (panel.node.kind === "host" || panel.node.kind === "internet")
                Layout.fillWidth: true
                spacing: 8
                Heading {
                    text: panel.node && panel.node.kind === "internet" ? "INTERNET" : "THIS COMPUTER"
                    Layout.topMargin: 0
                }
                Line {
                    text: panel.node && panel.node.kind === "internet" ? "Everything outside this computer. The VMs below can reach it." : "Your computer shares its internet connection with VMs that use a private internet connection or an “Internet + VMs” network. “This computer + VMs” networks stop here."
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Heading {
                    text: "VMS THAT CAN REACH IT"
                }
                Repeater {
                    model: panel.vms.filter(function (v) {
                        return panel.node && (panel.node.kind === "internet" ? v.reach === "internet" || v.reach === "lan" : v.reach !== "none" && v.reach !== "lab");
                    })
                    Line {
                        required property var modelData
                        text: "● " + modelData.label + (modelData.running ? "" : " (stopped)")
                        color: modelData.running ? theme.colors.warning : theme.colors.muted
                    }
                }
                AppButton {
                    text: "Cut off internet"
                    iconName: "unplug"
                    tone: "danger"
                    visible: !!panel.node && panel.node.kind === "internet"
                    onClicked: panel.map.killInternet()
                }
            }
        }
    }
}
