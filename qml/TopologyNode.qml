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
    readonly property color ink: !map ? theme.colors.border : kind === "internet" ? theme.colors.warning : kind === "host" ? theme.colors.warning : kind === "switch" ? map.uplinkInk(node.uplink) : reach ? reach.ink : theme.colors.muted
    readonly property string subtitle: kind === "internet" ? (node.exposed ? node.exposed + (node.exposed === 1 ? " running VM online" : " running VMs online") : "No running VM online") : kind === "host" ? "Shares its internet with VMs" : kind === "switch" ? (map ? map.kindText(node) : "") + (node.cidr ? " · " + node.cidr : "") : (node.running ? "Running" : String(node.state || "Stopped")) + (node.ip ? " · " + node.ip : node.running && node.privateOnly ? " · private address" : "")
    objectName: "topologyNode_" + nodeId
    width: map ? map.sizes[kind][0] : 200
    height: map ? map.sizes[kind][1] : 80
    radius: kind === "internet" ? height / 2 : 10
    // In the operations-center style, cards are darker and outlined in what they lead to.
    readonly property color face: map && map.operations ? "#0a111c" : theme.colors.surface
    color: dropTarget ? Qt.tint(face, map.tint(theme.colors.success, .14)) : chosen ? Qt.tint(face, map.tint(theme.colors.accent, .08)) : face
    border.width: chosen || dropTarget ? 2 : 1
    border.color: dropTarget ? theme.colors.success : chosen ? theme.colors.accent : hover.hovered ? map.tint(ink, .7) : map && map.operations ? map.tint(ink, .45) : Qt.tint(theme.colors.border, map ? map.tint(ink, .18) : "transparent")
    opacity: unreachable ? .28 : dimmed ? .7 : 1
    Behavior on opacity {
        NumberAnimation {
            duration: 140
        }
    }
    z: drag.active ? 5 : chosen ? 2 : 1
    Accessible.role: Accessible.Button
    Accessible.name: (node.label || "") + ", " + subtitle + (reach ? ", " + reach.text : "")
    Behavior on border.color {
        enabled: !theme.reducedMotion
        ColorAnimation {
            duration: 120
        }
    }

    gradient: Gradient {
        GradientStop {
            position: 0
            color: box.map && box.map.operations ? Qt.lighter(box.color, 1.25) : Qt.lighter(box.color, 1.06)
        }
        GradientStop {
            position: 1
            color: box.color
        }
    }
    // Soft shadow, so devices sit above the cables.
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.topMargin: 5
        anchors.leftMargin: 2
        anchors.rightMargin: -2
        anchors.bottomMargin: -6
        radius: parent.radius + 2
        color: "#000000"
        opacity: .22
    }
    // Halo in what the device leads to when it is selected, hovered or a place to drop a cable.
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.margins: box.chosen || box.dropTarget ? -6 : -3
        radius: parent.radius + (box.chosen || box.dropTarget ? 6 : 3)
        color: "transparent"
        border.width: box.chosen || box.dropTarget ? 4 : 2
        border.color: box.dropTarget ? theme.colors.success : box.chosen ? theme.colors.accent : box.ink
        opacity: box.dropTarget ? .5 : box.chosen ? .3 : hover.hovered ? .18 : 0
        Behavior on opacity {
            enabled: !theme.reducedMotion
            NumberAnimation {
                duration: 140
            }
        }
        SequentialAnimation on opacity {
            running: box.dropTarget && !theme.reducedMotion
            loops: Animation.Infinite
            NumberAnimation {
                to: .2
                duration: 420
            }
            NumberAnimation {
                to: .6
                duration: 420
            }
        }
    }
    // Thin highlight along the top, as if lit from above.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 1
        anchors.leftMargin: box.radius
        anchors.rightMargin: box.radius
        height: 1
        color: "#ffffff"
        opacity: box.map && box.map.operations ? .07 : .05
    }
    // Colored edge showing what this device leads to, with a soft glow on running devices.
    Rectangle {
        visible: box.kind !== "internet"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 1
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        width: 3
        radius: 1.5
        color: box.ink
        opacity: box.dimmed ? .45 : 1
        Rectangle {
            visible: !box.dimmed
            anchors.centerIn: parent
            width: 9
            height: parent.height + 4
            radius: 4.5
            color: box.ink
            opacity: .14
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: box.kind === "internet" ? 16 : 14
        anchors.rightMargin: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 11
        Rectangle {
            id: tile
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            radius: box.kind === "internet" ? 20 : 9
            color: box.map ? box.map.tint(box.ink, box.dimmed ? .08 : .15) : "transparent"
            border.width: 1
            border.color: box.map ? box.map.tint(box.ink, box.dimmed ? .15 : .35) : "transparent"
            AppIcon {
                anchors.centerIn: parent
                width: 22
                height: 22
                name: box.kind === "internet" ? "globe" : box.kind === "host" ? "router" : box.kind === "switch" ? "switch" : "monitor"
                color: box.ink
            }
            // The internet's orbit turns while any running VM can reach it.
            Canvas {
                id: orbit
                visible: box.kind === "internet"
                anchors.centerIn: parent
                width: 52
                height: 52
                readonly property bool active: box.kind === "internet" && !!box.node.exposed
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.strokeStyle = String(box.ink);
                    ctx.lineWidth = 1.5;
                    ctx.lineCap = "round";
                    ctx.globalAlpha = active ? .85 : .3;
                    ctx.beginPath();
                    ctx.arc(26, 26, 24, -Math.PI / 2, Math.PI * .35);
                    ctx.stroke();
                    ctx.globalAlpha = active ? .35 : .15;
                    ctx.beginPath();
                    ctx.arc(26, 26, 24, Math.PI * .55, Math.PI * 1.3);
                    ctx.stroke();
                    if (active) {
                        ctx.globalAlpha = 1;
                        ctx.fillStyle = String(box.ink);
                        ctx.beginPath();
                        ctx.arc(26 + 24 * Math.cos(Math.PI * .35), 26 + 24 * Math.sin(Math.PI * .35), 2.6, 0, Math.PI * 2);
                        ctx.fill();
                    }
                }
                onActiveChanged: requestPaint()
                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 6000
                    loops: Animation.Infinite
                    running: orbit.active && orbit.visible && !theme.reducedMotion
                }
                Connections {
                    target: theme
                    function onChanged() {
                        orbit.requestPaint();
                    }
                }
            }
            // Power light on VMs and networks; it breathes while the device runs.
            Rectangle {
                id: power
                visible: box.kind === "vm" || box.kind === "switch"
                readonly property bool on: box.kind === "switch" ? !!box.node.running : box.node.stateCode === 1
                x: parent.width - 8
                y: -4
                width: 12
                height: 12
                radius: 6
                border.width: 2
                border.color: box.face
                color: box.kind === "switch" ? (!box.node.running ? theme.colors.muted : box.node.usable ? theme.colors.success : theme.colors.warning) : box.node.stateCode === 1 ? theme.colors.success : box.node.stateCode === 3 ? theme.colors.warning : theme.colors.muted
                Rectangle {
                    z: -1
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    radius: 10
                    color: power.color
                    visible: power.on
                    SequentialAnimation on opacity {
                        running: power.on && !theme.reducedMotion && power.visible
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: .05
                            to: .35
                            duration: 1400
                            easing.type: Easing.InOutSine
                        }
                        NumberAnimation {
                            from: .35
                            to: .05
                            duration: 1400
                            easing.type: Easing.InOutSine
                        }
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            Label {
                textFormat: Text.PlainText
                text: box.node.label || ""
                font.weight: Font.DemiBold
                font.pixelSize: Math.round(14 * theme.textScale)
                color: theme.colors.foreground
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                objectName: "subtitle_" + box.nodeId
                textFormat: Text.PlainText
                text: box.subtitle
                font.pixelSize: Math.round(11 * theme.textScale)
                color: box.kind === "internet" && box.node.exposed ? theme.colors.warning : theme.colors.muted
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // Status pills: what a VM can reach, or a network's problems and isolation.
            RowLayout {
                visible: box.kind === "vm" || box.kind === "switch"
                spacing: 5
                Label {
                    objectName: "reach_" + box.nodeId
                    visible: text !== ""
                    text: box.kind === "vm" && box.reach ? "● " + box.reach.label : box.kind === "switch" && box.node.needsPermission ? "! NEEDS PERMISSION" : box.kind === "switch" && !box.node.running ? "STOPPED" : box.kind === "switch" && box.node.category === "Isolated" ? (box.node.sealed ? "✓ VERIFIED ISOLATED" : "✗ NOT VERIFIED") : box.kind === "switch" ? box.node.vmCount + (box.node.vmCount === 1 ? " VM" : " VMs") : ""
                    color: box.kind === "vm" && box.reach ? box.reach.ink : box.node.needsPermission ? theme.colors.warning : box.kind === "switch" && box.node.category === "Isolated" && box.node.running ? (box.node.sealed ? theme.colors.success : theme.colors.danger) : theme.colors.muted
                    font.pixelSize: Math.round(10 * theme.textScale)
                    font.weight: Font.Bold
                    font.letterSpacing: .5
                }
                Label {
                    visible: box.kind === "vm" && !!box.node.contained
                    text: "CONTAINED"
                    color: theme.colors.danger
                    font.pixelSize: Math.round(10 * theme.textScale)
                    font.weight: Font.Bold
                    font.letterSpacing: .5
                }
            }
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (box.map) {
            if (hovered)
                box.map.hoverNode = box.nodeId;
            else if (box.map.hoverNode === box.nodeId)
                box.map.hoverNode = "";
        }
    }
    DragHandler {
        id: drag
        target: box
        // Takes the drag from the map's own pan handler, so only this device moves.
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onActiveChanged: if (!active)
            box.map.remember(box.nodeId, box)
        else
            box.map.selected = box.nodeId
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: function (point, button) {
            box.map.selected = box.nodeId;
            box.map.selectedCable = "";
            if (button === Qt.RightButton)
                box.map.openMenu(box.node, box, point.position.x, point.position.y);
        }
        onDoubleTapped: function (point, button) {
            if (button !== Qt.LeftButton)
                return;
            if (box.kind === "vm")
                box.map.openVm(box.node.uuid);
            else if (box.kind === "switch" && box.node.network && box.node.managed && !box.node.running)
                box.map.editNetwork(box.node.network);
            else
                box.map.selected = box.nodeId;
        }
    }

    // Connector on the VM's right side: drag it onto a network (or this computer, for a private
    // internet connection). It shows on hover, so cables' own ports stay uncluttered.
    Rectangle {
        id: port
        objectName: "port_" + box.nodeId
        visible: box.kind === "vm" && !!box.node.owned
        anchors.verticalCenter: parent.verticalCenter
        x: parent.width - width / 2
        width: 18
        height: 18
        radius: 9
        color: portDrag.active ? theme.colors.accent : box.face
        border.width: 2
        border.color: portDrag.containsMouse || portDrag.active ? theme.colors.accent : box.map ? box.map.tint(box.ink, .9) : theme.colors.muted
        opacity: hover.hovered || portDrag.containsMouse || portDrag.active || box.chosen ? 1 : .55
        scale: portDrag.containsMouse ? 1.25 : 1
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation {
                duration: 90
            }
        }
        Behavior on opacity {
            enabled: !theme.reducedMotion
            NumberAnimation {
                duration: 120
            }
        }
        Label {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -1
            text: "+"
            font.pixelSize: 14
            font.weight: Font.Bold
            color: portDrag.active ? theme.colors.accentText : box.map ? box.map.tint(box.ink, .95) : theme.colors.muted
        }
        ToolTip.visible: portDrag.containsMouse && !portDrag.pressed
        ToolTip.delay: 400
        ToolTip.text: "Drag onto a network to connect " + (box.node.label || "this VM")
        // A MouseArea grabs on press, so the box's own DragHandler never starts moving the VM instead.
        MouseArea {
            id: portDrag
            readonly property bool active: pressed && !!box.map && !!box.map.wire
            anchors.fill: parent
            anchors.margins: -7
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.CrossCursor
            function track(mouse) {
                const w = mapToItem(box.parent, mouse.x, mouse.y);
                const over = box.map.nodeAt(w.x, w.y);
                const kind = over ? box.map.graph.byId[over].kind : "";
                box.map.wire = {
                    from: box.nodeId,
                    x: w.x,
                    y: w.y,
                    over: kind === "switch" || kind === "host" ? over : ""
                };
            }
            onPressed: function (mouse) {
                box.map.selected = box.nodeId;
                track(mouse);
            }
            onPositionChanged: function (mouse) {
                if (pressed)
                    track(mouse);
            }
            onReleased: {
                const wire = box.map.wire;
                if (wire && wire.over)
                    box.map.dropWire(box.nodeId, wire.over);
                box.map.wire = null;
            }
            onCanceled: box.map.wire = null
        }
    }
}
