// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One device on the network map: the internet, this computer, a network or a VM.
// Drag to move, double-click to open, right-click for actions; VMs have a port to drag new cables from.
Rectangle {
    id: box
    property var map: null
    property string nodeId: ""
    property var node: ({})
    property var live: null
    readonly property string kind: node.kind || "vm"
    readonly property bool chosen: !!map && map.selected === nodeId
    readonly property bool dropTarget: !!map && !!map.wire && map.wire.over === nodeId
    readonly property bool dimmed: (kind === "vm" || kind === "switch") && !node.running
    // Faded when a selected VM can't reach this device.
    readonly property bool unreachable: !!map && !!map.reachFocus && !map.reachFocus.nodes[nodeId]
    readonly property var reach: kind === "vm" && map ? map.reachInfo[node.reach || "none"] : null
    // The color of what this device leads to: amber internet, blue this computer, green VMs only.
    readonly property color ink: !map ? theme.colors.border
        : kind === "internet" ? theme.colors.warning
        : kind === "host" ? theme.colors.warning
        : kind === "switch" ? map.uplinkInk(node.uplink)
        : reach ? reach.ink : theme.colors.muted
    readonly property string subtitle: kind === "internet" ? (node.exposed ? node.exposed + (node.exposed === 1 ? " running VM online" : " running VMs online") : "No running VM online")
        : kind === "host" ? "Shares its internet with VMs"
        : kind === "switch" ? (map ? map.kindText(node) : "") + (node.cidr ? " · " + node.cidr : "")
        : (node.running ? "Running" : String(node.state || "Stopped")) + (node.ip ? " · " + node.ip : node.running && node.privateOnly ? " · private address" : "")
    objectName: "topologyNode_" + nodeId
    width: map ? map.sizes[kind][0] : 200
    height: map ? map.sizes[kind][1] : 80
    radius: kind === "internet" ? height / 2 : 8
    color: dropTarget ? Qt.tint(theme.colors.surface, map.tint(theme.colors.success, .14)) : chosen ? Qt.tint(theme.colors.surface, map.tint(theme.colors.accent, .08)) : theme.colors.surface
    border.width: chosen || dropTarget ? 2 : 1
    border.color: dropTarget ? theme.colors.success : chosen ? theme.colors.accent : hover.hovered ? map.tint(ink, .7) : theme.colors.border
    opacity: unreachable ? .28 : dimmed ? .7 : 1
    Behavior on opacity { NumberAnimation { duration: 140 } }
    z: drag.active ? 5 : chosen ? 2 : 1
    Accessible.role: Accessible.Button
    Accessible.name: (node.label || "") + ", " + subtitle + (reach ? ", " + reach.text : "")
    Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 120 } }

    // Soft shadow, so devices sit above the cables.
    Rectangle {
        z: -1; anchors.fill: parent; anchors.topMargin: 3; anchors.leftMargin: 1; anchors.rightMargin: -1; anchors.bottomMargin: -3
        radius: parent.radius; color: "#000000"; opacity: .18
    }
    // Colored edge showing what this device leads to.
    Rectangle {
        visible: box.kind !== "internet"
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.margins: 1
        width: 4; radius: 2
        color: box.ink; opacity: box.dimmed ? .5 : 1
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: box.kind === "internet" ? 16 : 14; anchors.rightMargin: 12
        anchors.topMargin: 8; anchors.bottomMargin: 8
        spacing: 11
        Rectangle {
            Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: box.kind === "internet" ? 19 : 8
            color: box.map ? box.map.tint(box.ink, .14) : "transparent"
            AppIcon {
                anchors.centerIn: parent; width: 22; height: 22
                name: box.kind === "internet" ? "globe" : box.kind === "host" ? "router" : box.kind === "switch" ? "switch" : "monitor"
                color: box.ink
            }
            // Power light on VMs and networks.
            Rectangle {
                visible: box.kind === "vm" || box.kind === "switch"
                x: parent.width - 7; y: -3; width: 11; height: 11; radius: 6
                border.width: 2; border.color: theme.colors.surface
                color: box.kind === "switch" ? (!box.node.running ? theme.colors.muted : box.node.usable ? theme.colors.success : theme.colors.warning)
                    : box.node.stateCode === 1 ? theme.colors.success : box.node.stateCode === 3 ? theme.colors.warning : theme.colors.muted
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            Label {
                textFormat: Text.PlainText
                text: box.node.label || ""
                font.weight: Font.DemiBold; font.pixelSize: Math.round(14 * theme.textScale)
                color: theme.colors.foreground; elide: Text.ElideRight; Layout.fillWidth: true
            }
            Label {
                objectName: "subtitle_" + box.nodeId
                textFormat: Text.PlainText
                text: box.subtitle
                font.pixelSize: Math.round(11 * theme.textScale)
                color: box.kind === "internet" && box.node.exposed ? theme.colors.warning : theme.colors.muted
                elide: Text.ElideRight; Layout.fillWidth: true
            }
            // Status pills: what a VM can reach, or a network's problems and isolation.
            RowLayout {
                visible: box.kind === "vm" || box.kind === "switch"
                spacing: 5
                Label {
                    objectName: "reach_" + box.nodeId
                    visible: text !== ""
                    text: box.kind === "vm" && box.reach ? "● " + box.reach.label
                        : box.kind === "switch" && box.node.needsPermission ? "! NEEDS PERMISSION"
                        : box.kind === "switch" && !box.node.running ? "STOPPED"
                        : box.kind === "switch" && box.node.category === "Isolated" ? (box.node.sealed ? "✓ VERIFIED ISOLATED" : "✗ NOT VERIFIED")
                        : box.kind === "switch" ? box.node.vmCount + (box.node.vmCount === 1 ? " VM" : " VMs") : ""
                    color: box.kind === "vm" && box.reach ? box.reach.ink
                        : box.node.needsPermission ? theme.colors.warning
                        : box.kind === "switch" && box.node.category === "Isolated" && box.node.running ? (box.node.sealed ? theme.colors.success : theme.colors.danger)
                        : theme.colors.muted
                    font.pixelSize: Math.round(10 * theme.textScale); font.weight: Font.Bold; font.letterSpacing: .5
                }
                Label {
                    visible: box.kind === "vm" && !!box.node.contained
                    text: "CONTAINED"; color: theme.colors.danger
                    font.pixelSize: Math.round(10 * theme.textScale); font.weight: Font.Bold; font.letterSpacing: .5
                }
            }
        }
    }

    HoverHandler { id: hover }
    DragHandler {
        id: drag
        target: box
        onActiveChanged: if (!active) box.map.remember(box.nodeId, box); else box.map.selected = box.nodeId
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: function(point, button) {
            box.map.selected = box.nodeId; box.map.selectedCable = ""
            if (button === Qt.RightButton) box.map.openMenu(box.node, box, point.position.x, point.position.y)
        }
        onDoubleTapped: function(point, button) {
            if (button !== Qt.LeftButton) return
            if (box.kind === "vm") box.map.openVm(box.node.uuid)
            else if (box.kind === "switch" && box.node.network && box.node.managed && !box.node.running) box.map.editNetwork(box.node.network)
            else box.map.selected = box.nodeId
        }
    }

    // Cable port: drag it onto a network (or this computer, for a private internet connection).
    Rectangle {
        id: port
        objectName: "port_" + box.nodeId
        visible: box.kind === "vm" && !!box.node.owned
        anchors.horizontalCenter: parent.horizontalCenter
        y: -height / 2
        width: 16; height: 16; radius: 8
        color: portDrag.active ? theme.colors.accent : theme.colors.surface
        border.width: 2; border.color: portDrag.containsMouse || portDrag.active ? theme.colors.accent : box.map ? box.map.tint(box.ink, .9) : theme.colors.muted
        scale: portDrag.containsMouse ? 1.3 : 1
        Behavior on scale { enabled: !theme.reducedMotion; NumberAnimation { duration: 90 } }
        Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3; color: portDrag.active ? theme.colors.accentText : box.map ? box.map.tint(box.ink, .9) : theme.colors.muted }
        ToolTip.visible: portDrag.containsMouse && !portDrag.pressed
        ToolTip.delay: 400
        ToolTip.text: "Drag onto a network to connect " + (box.node.label || "this VM")
        // A MouseArea grabs on press, so the box's own DragHandler never starts moving the VM instead.
        MouseArea {
            id: portDrag
            readonly property bool active: pressed && !!box.map && !!box.map.wire
            anchors.fill: parent; anchors.margins: -7
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.CrossCursor
            function track(mouse) {
                const w = mapToItem(box.parent, mouse.x, mouse.y)
                const over = box.map.nodeAt(w.x, w.y)
                const kind = over ? box.map.graph.byId[over].kind : ""
                box.map.wire = {from: box.nodeId, x: w.x, y: w.y, over: kind === "switch" || kind === "host" ? over : ""}
            }
            onPressed: function(mouse) { box.map.selected = box.nodeId; track(mouse) }
            onPositionChanged: function(mouse) { if (pressed) track(mouse) }
            onReleased: {
                const wire = box.map.wire
                if (wire && wire.over) box.map.dropWire(box.nodeId, wire.over)
                box.map.wire = null
            }
            onCanceled: box.map.wire = null
        }
    }
}
